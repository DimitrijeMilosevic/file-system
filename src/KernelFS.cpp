#include "KernelFS.h"
#include "file.h"
#include "filedesc.h"

const unsigned int KernelFS::LVL1_ENTRY_SIZE_IN_BYTES = 4;
const unsigned int KernelFS::LVL2_ENTRY_SIZE_IN_BYTES = 4;
const unsigned int KernelFS::FILEDESC_ENTRY_SIZE_IN_BYTES = 32;

const unsigned int KernelFS::FILE_NAME_OFFSET = 0;
const unsigned int KernelFS::FILE_EXTENSION_OFFSET = 8;
const unsigned int KernelFS::NOT_IN_USE_BYTE_OFFSET = 11;
const unsigned int KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET = 12;
const unsigned int KernelFS::FILE_SIZE_OFFSET = 16;

HANDLE KernelFS::ok_to_mount = CreateSemaphore(NULL, 1, 32, NULL);
HANDLE KernelFS::ok_to_unmount = CreateSemaphore(NULL, 0, 32, NULL);
HANDLE KernelFS::ok_to_format = CreateSemaphore(NULL, 0, 32, NULL);
SRWLOCK KernelFS::srwLock = SRWLOCK_INIT;
int KernelFS::waitingToUnMount = 0;
int KernelFS::waitingToFormat = 0;
long KernelFS::numberOfOpenedFiles = 0;
int KernelFS::numOfClusters = 0;
int KernelFS::bitVectorSizeInClusters = 0;
int KernelFS::rootLvl1IndexClusterNo = 0;
Partition* KernelFS::mountedPartition = nullptr;
std::unordered_map<Partition*, bool> KernelFS::formattedPartitions = std::unordered_map<Partition*, bool>();
std::unordered_map<std::string, FileDesc*> KernelFS::files = std::unordered_map<std::string, FileDesc*>();

char KernelFS::mount(Partition* partition) {
	if (partition == nullptr) return 0;
	WaitForSingleObject(
		ok_to_mount, // handle to semaphore
		INFINITE // infinite time-out interval
	);
	AcquireSRWLockExclusive(&srwLock);
	KernelFS::mountedPartition = partition;
	if (KernelFS::formattedPartitions.find(partition) == KernelFS::formattedPartitions.end())
		KernelFS::formattedPartitions.insert(std::make_pair(partition, false));
	ReleaseSRWLockExclusive(&srwLock);
	return 1;
}

char KernelFS::unmount() {
	AcquireSRWLockExclusive(&srwLock);
	if (KernelFS::mountedPartition == nullptr) {
		ReleaseSRWLockExclusive(&srwLock);
		return 0;
	}
	if (KernelFS::numberOfOpenedFiles > 0) {
		KernelFS::waitingToUnMount++;
		ReleaseSRWLockExclusive(&srwLock);
		WaitForSingleObject(
			ok_to_unmount, // handle to semaphore
			INFINITE // infinite time-out interval
		);
		AcquireSRWLockExclusive(&srwLock);
		KernelFS::waitingToUnMount--;
		if (KernelFS::mountedPartition == nullptr) {// someone already unmounted the partition
			ReleaseSRWLockExclusive(&srwLock);
			return 1;
		}
	}
	KernelFS::mountedPartition = nullptr;
	KernelFS::numberOfOpenedFiles = 0; // reset the number of opened files on the mounted partition (is this necessary?)
	KernelFS::numOfClusters = 0;
	KernelFS::bitVectorSizeInClusters = 0;
	KernelFS::rootLvl1IndexClusterNo = 0;
	// invalidate the files map
	KernelFS::files.clear();
	// potentially unblock the threads that are waiting to format the partition, to prevent deadlock
	ReleaseSemaphore(
		ok_to_format, // handle to semaphore
		KernelFS::waitingToFormat, // increase count by 1
		NULL // not interested in previous count
	);
	ReleaseSRWLockExclusive(&srwLock);
	// potentially unblock the thread that is waiting to mount a partition / allow upcoming threads to mount a partition
	ReleaseSemaphore(
		ok_to_mount, // handle to semaphore
		1, // increase count by 1
		NULL // not interested in previous count
	);
	return 1;
}

void KernelFS::initializeBitVector() {
	// how much clusters does evidenting bit vector's clusters take up in the bit vector
	int numOfBitVectorClusters = KernelFS::bitVectorSizeInClusters / (ClusterSize * CHAR_SIZE_IN_BITS)
		+ (((KernelFS::bitVectorSizeInClusters % (ClusterSize * CHAR_SIZE_IN_BITS)) > 0) ? 1 : 0);
	if (numOfBitVectorClusters > 1) {
		char emptyCluster[2048];
		for (int i = 0; i < 2048; i++)
			emptyCluster[i] = 0x00;
		for (int clusterNo = 0; clusterNo < numOfBitVectorClusters - 1; clusterNo++)
			KernelFS::mountedPartition->writeCluster(clusterNo, emptyCluster);
	}
	char bitVectorCluster[2048];
	// how much bits are left for evidenting bit vector's clusters in the last of the clusters used to evident bit vector's clusters
	int bitsLeft = KernelFS::bitVectorSizeInClusters % (ClusterSize * CHAR_SIZE_IN_BITS);
	// number of the last of the clusters used to evident bit vector's clusters
	int nextClusterNo = (numOfBitVectorClusters == 1) ? 0 : (numOfBitVectorClusters - 1);
	// fill the whole bytes first
	int byteNo;
	for (byteNo = 0; byteNo < (bitsLeft / CHAR_SIZE_IN_BITS); byteNo++)
		bitVectorCluster[byteNo] = 0x00;
	if (byteNo < 2048) {
		// fill the remaining bits
		int bitsNo = bitsLeft % CHAR_SIZE_IN_BITS;
		char tmp = 0xff;
		for (int bitNo = 0; bitNo < bitsNo; bitNo++)
			tmp &= ~(1 << bitNo);
		bitVectorCluster[byteNo++] = tmp;
	}
	// remaining bytes - free clusters
	for (; byteNo < 2048; byteNo++)
		bitVectorCluster[byteNo] = 0xff;
	// write the last of the clusters used to evident bit vector's clusters
	KernelFS::mountedPartition->writeCluster(nextClusterNo, bitVectorCluster);
	// how much more clusters are needed to be written
	int remaining = KernelFS::bitVectorSizeInClusters - numOfBitVectorClusters;
	if (remaining > 0) {
		char allFreeCluster[2048];
		int i;
		for (int i = 0; i < 2048; i++)
			allFreeCluster[i] = 0xff;
		for (i = 0; i < remaining; i++)
			KernelFS::mountedPartition->writeCluster(numOfBitVectorClusters + i, allFreeCluster);
	}
	// evidenting root directory's level 1 index in the bit vector;
	int clustNo = KernelFS::rootLvl1IndexClusterNo / (ClusterSize * CHAR_SIZE_IN_BITS);
	int bytNo = (KernelFS::rootLvl1IndexClusterNo % (ClusterSize * CHAR_SIZE_IN_BITS)) / CHAR_SIZE_IN_BITS;
	int bitNo = (KernelFS::rootLvl1IndexClusterNo % (ClusterSize * CHAR_SIZE_IN_BITS)) % CHAR_SIZE_IN_BITS;
	char cluster[2048];
	KernelFS::mountedPartition->readCluster(clustNo, cluster);
	cluster[bytNo] &= ~(1 << bitNo);
	KernelFS::mountedPartition->writeCluster(clustNo, cluster);
}

char KernelFS::format() {
	AcquireSRWLockExclusive(&srwLock);
	if (KernelFS::mountedPartition == nullptr) {
		ReleaseSRWLockExclusive(&srwLock);
		return 0;
	}
	if (KernelFS::numberOfOpenedFiles > 0) {
		KernelFS::waitingToFormat++;
		ReleaseSRWLockExclusive(&srwLock);
		WaitForSingleObject(
			ok_to_format, // handle to semaphore
			INFINITE // infinite time-out interval
		);
		AcquireSRWLockExclusive(&srwLock);
		KernelFS::waitingToFormat--;
		if (KernelFS::mountedPartition == nullptr) {// someone already unmounted the partition
			ReleaseSRWLockExclusive(&srwLock);
			return 0;
		}
	}
	KernelFS::numOfClusters = KernelFS::mountedPartition->getNumOfClusters();
	KernelFS::bitVectorSizeInClusters = KernelFS::numOfClusters / (ClusterSize * CHAR_SIZE_IN_BITS) 
		+ ((KernelFS::numOfClusters % (ClusterSize * CHAR_SIZE_IN_BITS) > 0) ? 1 : 0);
	KernelFS::rootLvl1IndexClusterNo = KernelFS::bitVectorSizeInClusters;
	if (KernelFS::formattedPartitions[KernelFS::mountedPartition] == true) {
		ReleaseSRWLockExclusive(&srwLock);
		return 0;
	}
	KernelFS::initializeBitVector();
	// initialization of the first-level index cluster of the root directory
	char emptyBuffer[2048];
	for (int i = 0; i < 2048; i++)
		emptyBuffer[i] = 0x00;
	KernelFS::mountedPartition->writeCluster(KernelFS::rootLvl1IndexClusterNo, emptyBuffer);
	// evidenting of the root directory's level 1 index cluster inside of the bit vector was done in KernelFS::initializeBitVector()...
	// end of initialization of the first-level index cluster of the root directory
	KernelFS::formattedPartitions[KernelFS::mountedPartition] = true;
	ReleaseSRWLockExclusive(&srwLock);
	return 1;
}

FileCnt KernelFS::readRootDir() {
	AcquireSRWLockShared(&srwLock);
	if (KernelFS::mountedPartition == nullptr || KernelFS::formattedPartitions[mountedPartition] == false) {
		ReleaseSRWLockShared(&srwLock);
		return -1;
	}
	char bufferedRootDir[2048];
	char bufferedLvl2IndexCluster[2048];
	char bufferedFileDescCluster[2048];
	ClusterNo clusterNo;
	FileCnt fileCount = 0;
	KernelFS::mountedPartition->readCluster(KernelFS::rootLvl1IndexClusterNo, bufferedRootDir);
	for (int lvl1Entry = 0; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		clusterNo = 0;
		clusterNo |= bufferedRootDir[lvl1Entry + 0];
		clusterNo |= bufferedRootDir[lvl1Entry + 1] << 8;
		clusterNo |= bufferedRootDir[lvl1Entry + 2] << 16;
		clusterNo |= bufferedRootDir[lvl1Entry + 3] << 24;
		if (clusterNo == 0) continue; // no level 2 index cluster
		KernelFS::mountedPartition->readCluster(clusterNo, bufferedLvl2IndexCluster);
		for (int lvl2Entry = 0; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			clusterNo = 0;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 0];
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 1] << 8;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 2] << 16;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 3] << 24;
			if (clusterNo == 0) continue; // no file descriptor cluster 
			KernelFS::mountedPartition->readCluster(clusterNo, bufferedFileDescCluster);
			for (int fileDescEntry = 0; fileDescEntry < 2048; fileDescEntry += KernelFS::FILEDESC_ENTRY_SIZE_IN_BYTES)
				if (bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET] != 0x00)
					fileCount++;
		}
	}
	ReleaseSRWLockShared(&srwLock);
	return fileCount;
}

char KernelFS::doesExist(char* fname) {
	if (fname == nullptr) return -1;
	AcquireSRWLockShared(&srwLock);
	if (KernelFS::mountedPartition == nullptr || KernelFS::formattedPartitions[KernelFS::mountedPartition] == false) {
		ReleaseSRWLockShared(&srwLock);
		return -1;
	}
	if (KernelFS::files.find((std::string)fname) != KernelFS::files.end()) {
		ReleaseSRWLockExclusive(&srwLock);
		return 1; // file found in the files map; that means the file's descriptor has been cached - file exists
	}
	char bufferedRootDir[2048];
	char bufferedLvl2IndexCluster[2048];
	char bufferedFileDescCluster[2048];
	ClusterNo clusterNo;
	KernelFS::mountedPartition->readCluster(KernelFS::rootLvl1IndexClusterNo, bufferedRootDir);
	for (int lvl1Entry = 0; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		clusterNo = 0;
		clusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 0]);
		clusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 1]) << 8;
		clusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 2]) << 16;
		clusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 3]) << 24;
		if (clusterNo == 0) continue; // no level 2 index cluster
		KernelFS::mountedPartition->readCluster(clusterNo, bufferedLvl2IndexCluster);
		for (int lvl2Entry = 0; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			clusterNo = 0;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 0];
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 1] << 8;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 2] << 16;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 3] << 24;
			if (clusterNo == 0) continue; // no file descriptor cluster 
			KernelFS::mountedPartition->readCluster(clusterNo, bufferedFileDescCluster);
			for (int fileDescEntry = 0; fileDescEntry < 2048; fileDescEntry += KernelFS::FILEDESC_ENTRY_SIZE_IN_BYTES) {
				// if the first byte in file name's 8 bytes equals 0x00, it means that the entry does not hold information about a file
				if (bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET + 0] == 0x00) continue; // no file descriptor
				std::string fullFileName = "/";
				int offset;
				// first 8 bytes in the entry represent file name
				for (offset = 0; offset < 8 && bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET + offset] != ' '; offset++)
					fullFileName += bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET + offset];
				fullFileName += ".";
				// next 3 bytes in the entry represent file extension
				for (offset = 0; offset < 3 && bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_EXTENSION_OFFSET + offset] != ' '; offset++)
					fullFileName += bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_EXTENSION_OFFSET + offset];
				if (fullFileName == (std::string)fname) {
					char fileDesc[KernelFS::LVL2_ENTRY_SIZE_IN_BYTES];
					for (offset = 0; offset < KernelFS::LVL2_ENTRY_SIZE_IN_BYTES; offset++)
						fileDesc[offset] = bufferedFileDescCluster[fileDescEntry + offset];
					ReleaseSRWLockShared(&srwLock);
					return 1; // file found - matching file name and file extension
				}
			}
		}	
	}
	ReleaseSRWLockShared(&srwLock);
	return 0; // file not found
}

FileDesc* KernelFS::getFileDescriptor(char* fname) {
	if (KernelFS::files.find((std::string)fname) != KernelFS::files.end())
		return KernelFS::files[(std::string)fname];
	char bufferedRootDir[2048];
	char bufferedLvl2IndexCluster[2048];
	char bufferedFileDescCluster[2048];
	ClusterNo clusterNo;
	KernelFS::mountedPartition->readCluster(KernelFS::rootLvl1IndexClusterNo, bufferedRootDir);
	for (int lvl1Entry = 0; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		clusterNo = 0;
		clusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 0]);
		clusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 1]) << 8;
		clusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 2]) << 16;
		clusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 3]) << 24;
		if (clusterNo == 0) continue; // no level 2 index cluster
		KernelFS::mountedPartition->readCluster(clusterNo, bufferedLvl2IndexCluster);
		for (int lvl2Entry = 0; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			clusterNo = 0;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 0];
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 1] << 8;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 2] << 16;
			clusterNo |= bufferedLvl2IndexCluster[lvl2Entry + 3] << 24;
			if (clusterNo == 0) continue; // no file descriptor cluster 
			KernelFS::mountedPartition->readCluster(clusterNo, bufferedFileDescCluster);
			for (int fileDescEntry = 0; fileDescEntry < 2048; fileDescEntry += KernelFS::FILEDESC_ENTRY_SIZE_IN_BYTES) {
				// if the first byte in file name's 8 bytes equals 0x00, it means that the entry does not hold information about a file
				if (bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET + 0] == 0x00) continue; // no file descriptor
				std::string fullFileName = "/";
				int offset;
				// first 8 bytes in the entry represent file name
				for (offset = 0; offset < 8 && bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET + offset] != ' '; offset++)
					fullFileName += bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET + offset];
				fullFileName += ".";
				// next 3 bytes in the entry represent file extension
				for (offset = 0; offset < 3 && bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_EXTENSION_OFFSET + offset] != ' '; offset++)
					fullFileName += bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_EXTENSION_OFFSET + offset];
				if (fullFileName == (std::string)fname) {
					FileDesc* fd = new FileDesc(clusterNo, fileDescEntry);
					KernelFS::files.insert(std::make_pair(fullFileName, fd));
					return fd;
				}
			}
		}
	}
	return 0; // file not found
}

char KernelFS::format(char* fname, char* fileName, char* fileExtension) {
	if (fname[0] != '/') return 0; // non-absolute path
	if (fname[1] == '.') return 0; // no file name
	int i, j, k;
	for (i = 0; i < 8; i++)
		if (fname[i + 1] == '.')
			break;
		else
			fileName[i] = fname[i + 1];
	if (fname[i + 1] == '.') {
		j = i + 2; // first character after the '.'
		if (fname[j] == '\0') return 0; // no file extension
		if (i < 8)
			for (; i < 8; i++)
				fileName[i] = ' ';
		for (k = 0; k < 3; k++)
			if (fname[j + k] == '\0')
				break;
			else
				fileExtension[k] = fname[j + k];
		if (fname[j + k] != '\0') return 0; // file extension longer than 3 characters
		if (k < 3)
			for (; k < 3; k++)
				fileExtension[k] = ' ';
		return 1;
	}
	else
		return 0; // file name longer than 8 characters
}

ClusterNo KernelFS::allocateCluster() {
	char bitVectorCluster[2048];
	for (int clusterNo = 0; clusterNo < KernelFS::bitVectorSizeInClusters; clusterNo++) {
		KernelFS::mountedPartition->readCluster(0 + clusterNo, bitVectorCluster);
		for (int i = 0; i < 2048; i++) 
			for (int bitNo = 0; bitNo < 8; bitNo++)
				if ((bitVectorCluster[i] & (1 << bitNo)) != 0) { // cluster is free
					bitVectorCluster[i] &= ~(1 << bitNo);
					KernelFS::mountedPartition->writeCluster(0 + clusterNo, bitVectorCluster);
					return clusterNo * ClusterSize * CHAR_SIZE_IN_BITS + (i << 3) + bitNo;
				}
	}
	return 0; // no free cluster found
}

char KernelFS::allocateFileDescriptor(std::string fname, char* fileName, char* fileExtension) {
	char bufferedRootDir[2048];
	KernelFS::mountedPartition->readCluster(KernelFS::rootLvl1IndexClusterNo, bufferedRootDir);
	char bufferedLvl2IndexCluster[2048];
	char bufferedFileDescCluster[2048];
	char emptyCluster[2048];
	for (int i = 0; i < 2048; i++)
		emptyCluster[i] = 0x00;
	for (int lvl1Entry = 0; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		ClusterNo lvl2IndexClusterNo = 0;
		lvl2IndexClusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 0]);
		lvl2IndexClusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 1]) << 8;
		lvl2IndexClusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 2]) << 16;
		lvl2IndexClusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 3]) << 24;
		if (lvl2IndexClusterNo == 0) continue; // no level 2 index cluster
		KernelFS::mountedPartition->readCluster(lvl2IndexClusterNo, bufferedLvl2IndexCluster);
		for (int lvl2Entry = 0; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			ClusterNo fileDescClusterNo = 0;
			fileDescClusterNo |= ((unsigned char)bufferedLvl2IndexCluster[lvl2Entry + 0]);
			fileDescClusterNo |= ((unsigned char)bufferedLvl2IndexCluster[lvl2Entry + 1]) << 8;
			fileDescClusterNo |= ((unsigned char)bufferedLvl2IndexCluster[lvl2Entry + 2]) << 16;
			fileDescClusterNo |= ((unsigned char)bufferedLvl2IndexCluster[lvl2Entry + 3]) << 24;
			if (fileDescClusterNo == 0) continue; // no file descriptor cluster
			KernelFS::mountedPartition->readCluster(fileDescClusterNo, bufferedFileDescCluster);
			for (int fileDescEntry = 0; fileDescEntry < 2048; fileDescEntry += KernelFS::FILEDESC_ENTRY_SIZE_IN_BYTES)
				if (bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET + 0] == 0x00) { // found a free file descriptor entry
					int offset;
					for (offset = 0; offset < 8; offset++)
						bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_NAME_OFFSET + offset] = fileName[offset];
					for (offset = 0; offset < 3; offset++)
						bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_EXTENSION_OFFSET + offset] = fileExtension[offset];
					bufferedFileDescCluster[fileDescEntry + KernelFS::NOT_IN_USE_BYTE_OFFSET] = 0x00;
					ClusterNo fileLvl1IndexClusterNo = KernelFS::allocateCluster();
					if (fileLvl1IndexClusterNo == 0 || (fileLvl1IndexClusterNo > KernelFS::numOfClusters - 1)) return 0; // no free cluster found
					bufferedFileDescCluster[fileDescEntry + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 0] = fileLvl1IndexClusterNo & 0xffUL;
					bufferedFileDescCluster[fileDescEntry + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 1] = (fileLvl1IndexClusterNo >> 8) & 0xffUL;
					bufferedFileDescCluster[fileDescEntry + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 2] = (fileLvl1IndexClusterNo >> 16) & 0xffUL;
					bufferedFileDescCluster[fileDescEntry + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 3] = (fileLvl1IndexClusterNo >> 24) & 0xffUL;
					for (offset = 0; offset < 4; offset++) // file size takes 4 bytes
						bufferedFileDescCluster[fileDescEntry + KernelFS::FILE_SIZE_OFFSET + offset] = 0x00;
					// writing all clusters back
					KernelFS::mountedPartition->writeCluster(fileLvl1IndexClusterNo, emptyCluster); // initialize file's level 1 index cluster
					KernelFS::mountedPartition->writeCluster(fileDescClusterNo, bufferedFileDescCluster); // write back the updated file descriptor cluster
					// all clusters written back
					KernelFS::files.insert(std::make_pair(fname, new FileDesc(fileDescClusterNo, fileDescEntry)));
					return 1;
				}
		}
		// no free file descriptor entries found in already allocated file descriptor clusters (for this level 2 index cluster)
		// find a free entry inside of the root directory's level 2 index cluster, if any
		int freeLvl2IndexClusterEntryNo = -1;
		for (int lvl2Entry = 0; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			ClusterNo fileDescClusterNo = 0;
			fileDescClusterNo |= ((unsigned char)bufferedLvl2IndexCluster[lvl2Entry + 0]);
			fileDescClusterNo |= ((unsigned char)bufferedLvl2IndexCluster[lvl2Entry + 1]) << 8;
			fileDescClusterNo |= ((unsigned char)bufferedLvl2IndexCluster[lvl2Entry + 2]) << 16;
			fileDescClusterNo |= ((unsigned char)bufferedLvl2IndexCluster[lvl2Entry + 3]) << 24;
			if (fileDescClusterNo == 0) { // found a free entry inside of the root directory's level 2 index cluster
				freeLvl2IndexClusterEntryNo = lvl2Entry;
				break;
			}
		}
		if (freeLvl2IndexClusterEntryNo != -1) {
			ClusterNo fileDescClusterNo = KernelFS::allocateCluster();
			if (fileDescClusterNo == 0 || (fileDescClusterNo > KernelFS::numOfClusters - 1)) return 0; // no free cluster found
			// form the file descriptor
			char fileDescCluster[2048];
			for (int i = 0; i < 2048; i++)
				fileDescCluster[i] = emptyCluster[i];
			int offset;
			for (offset = 0; offset < 8; offset++)
				fileDescCluster[0 + KernelFS::FILE_NAME_OFFSET + offset] = fileName[offset];
			for (offset = 0; offset < 3; offset++)
				fileDescCluster[0 + KernelFS::FILE_EXTENSION_OFFSET + offset] = fileExtension[offset];
			bufferedFileDescCluster[KernelFS::NOT_IN_USE_BYTE_OFFSET] = 0x00;
			ClusterNo fileLvl1IndexClusterNo = KernelFS::allocateCluster();
			if (fileLvl1IndexClusterNo == 0 || (fileLvl1IndexClusterNo > KernelFS::numOfClusters - 1)) return 0; // no free cluster found
			fileDescCluster[0 + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 0] = fileLvl1IndexClusterNo & 0xffUL;
			fileDescCluster[0 + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 1] = (fileLvl1IndexClusterNo >> 8) & 0xffUL;
			fileDescCluster[0 + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 2] = (fileLvl1IndexClusterNo >> 16) & 0xffUL;
			fileDescCluster[0 + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 3] = (fileLvl1IndexClusterNo >> 24) & 0xffUL;
			for (offset = 0; offset < 4; offset++) // file size takes 4 bytes
				fileDescCluster[0 + KernelFS::FILE_SIZE_OFFSET + offset] = 0x00;
			// file descriptor formed	
			// update root directory's level 2 index cluster entry
			bufferedLvl2IndexCluster[freeLvl2IndexClusterEntryNo + 0] = fileDescClusterNo & 0xffUL;
			bufferedLvl2IndexCluster[freeLvl2IndexClusterEntryNo + 1] = (fileDescClusterNo >> 8) & 0xffUL;
			bufferedLvl2IndexCluster[freeLvl2IndexClusterEntryNo + 2] = (fileDescClusterNo >> 16) & 0xffUL;
			bufferedLvl2IndexCluster[freeLvl2IndexClusterEntryNo + 3] = (fileDescClusterNo >> 24) & 0xffUL;
			// writing all clusters back
			KernelFS::mountedPartition->writeCluster(fileLvl1IndexClusterNo, emptyCluster); // initialize file's level 1 index cluster
			KernelFS::mountedPartition->writeCluster(fileDescClusterNo, fileDescCluster); // write the new file descriptor cluster
			KernelFS::mountedPartition->writeCluster(lvl2IndexClusterNo, bufferedLvl2IndexCluster); // write back the updated level 2 index cluster
			// all clusters written back
			// root directory's level 2 index cluster entry updated
			KernelFS::files.insert(std::make_pair(fname, new FileDesc(fileDescClusterNo, 0)));
			return 1;
		}
	}
	// no free file descriptor spot found inside of all of the allocated file descriptor clusters (for all level 2 index clusters allocated)
	// find a free entry inside of the root directory's level 1 index cluster
	int freeLvl1IndexClusterEntryNo = -1;
	for (int lvl1Entry = 0; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		ClusterNo lvl2IndexClusterNo = 0;
		lvl2IndexClusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 0]);
		lvl2IndexClusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 1]) << 8;
		lvl2IndexClusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 2]) << 16;
		lvl2IndexClusterNo |= ((unsigned char)bufferedRootDir[lvl1Entry + 3]) << 24;
		if (lvl2IndexClusterNo == 0) { // found a free entry inside of the root directory's level 1 index cluster
			freeLvl1IndexClusterEntryNo = lvl1Entry;
			break;
		}
	}
	if (freeLvl1IndexClusterEntryNo == -1) return 0; // no more level 2 index clusters can be allocated
	ClusterNo lvl2IndexClusterNo = KernelFS::allocateCluster();
	if (lvl2IndexClusterNo == 0 || (lvl2IndexClusterNo > KernelFS::numOfClusters - 1)) return 0; // no free cluster found
	char lvl2IndexCluster[2048]; // initially empty
	for (int i = 0; i < 2048; i++)
		lvl2IndexCluster[i] = emptyCluster[i];
	// update root directory's free entry
	bufferedRootDir[freeLvl1IndexClusterEntryNo + 0] = lvl2IndexClusterNo & 0xffUL;
	bufferedRootDir[freeLvl1IndexClusterEntryNo + 1] = (lvl2IndexClusterNo >> 8) & 0xffUL;
	bufferedRootDir[freeLvl1IndexClusterEntryNo + 2] = (lvl2IndexClusterNo >> 16) & 0xffUL;
	bufferedRootDir[freeLvl1IndexClusterEntryNo + 3] = (lvl2IndexClusterNo >> 24) & 0xffUL;
	ClusterNo fileDescClusterNo = KernelFS::allocateCluster();
	if (fileDescClusterNo == 0 || (fileDescClusterNo > (KernelFS::numOfClusters - 1))) return 0; // no free cluster found
	char fileDescCluster[2048];
	for (int i = 0; i < 2048; i++)
		fileDescCluster[i] = 0x00;
	int offset;
	for (offset = 0; offset < 8; offset++)
		fileDescCluster[0 + KernelFS::FILE_NAME_OFFSET + offset] = fileName[offset];
	for (offset = 0; offset < 3; offset++)
		fileDescCluster[0 + KernelFS::FILE_EXTENSION_OFFSET + offset] = fileExtension[offset];
	fileDescCluster[KernelFS::NOT_IN_USE_BYTE_OFFSET] = 0x00;
	ClusterNo fileLvl1IndexClusterNo = KernelFS::allocateCluster();
	if (fileLvl1IndexClusterNo == 0 || (fileLvl1IndexClusterNo > KernelFS::numOfClusters - 1)) return 0; // no free cluster found
	fileDescCluster[0 + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 0] = fileLvl1IndexClusterNo & 0xffUL;
	fileDescCluster[0 + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 1] = (fileLvl1IndexClusterNo >> 8) & 0xffUL;
	fileDescCluster[0 + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 2] = (fileLvl1IndexClusterNo >> 16) & 0xffUL;
	fileDescCluster[0 + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 3] = (fileLvl1IndexClusterNo >> 24) & 0xffUL;
	for (offset = 0; offset < 4; offset++) // file size takes 4 bytes
		fileDescCluster[0 + KernelFS::FILE_SIZE_OFFSET + offset] = 0x00;
	// file descriptor formed
	// update root directory's level 2 index cluster entry
	lvl2IndexCluster[0 + 0] = fileDescClusterNo & 0xffUL;
	lvl2IndexCluster[0 + 1] = (fileDescClusterNo >> 8) & 0xffUL;
	lvl2IndexCluster[0 + 2] = (fileDescClusterNo >> 16) & 0xffUL;
	lvl2IndexCluster[0 + 3] = (fileDescClusterNo >> 24) & 0xffUL;
	// writing all clusters back
	KernelFS::mountedPartition->writeCluster(fileLvl1IndexClusterNo, emptyCluster); // initialize file's level 1 index cluster
	KernelFS::mountedPartition->writeCluster(fileDescClusterNo, fileDescCluster); // write the new file descriptor cluster
	KernelFS::mountedPartition->writeCluster(lvl2IndexClusterNo, lvl2IndexCluster); // write the new level 2 index cluster
	KernelFS::mountedPartition->writeCluster(KernelFS::rootLvl1IndexClusterNo, bufferedRootDir); // write back the updated root dir
	// all clusters written back
	KernelFS::files.insert(std::make_pair(fname, new FileDesc(fileDescClusterNo, 0)));
	return 1;
}


File* KernelFS::open(char* fname, char mode) {
	if (fname == nullptr || (mode != 'r' && mode != 'w' && mode != 'a')) return nullptr;
	char fileName[8];
	char fileExtension[3];
	if (KernelFS::format(fname, fileName, fileExtension) == 0) return nullptr;
	AcquireSRWLockExclusive(&srwLock);
	if (KernelFS::mountedPartition == nullptr || KernelFS::formattedPartitions[KernelFS::mountedPartition] == false) {
		ReleaseSRWLockExclusive(&srwLock);
		return nullptr;
	}
	FileDesc* fileDescriptor = nullptr;
	if ((fileDescriptor = KernelFS::getFileDescriptor(fname)) == nullptr) { // file does not exist
		if (mode == 'r' || mode == 'a') { // file must exist to be opened in 'r'/'a' modes
			ReleaseSRWLockExclusive(&srwLock);
			return nullptr;
		}
		if (KernelFS::allocateFileDescriptor((std::string)fname, fileName, fileExtension) == 0) {
			ReleaseSRWLockExclusive(&srwLock);
			return nullptr; // couldn't create the file descriptor
		}
		fileDescriptor = KernelFS::files[(std::string)fname];
		File* file = new File((std::string)fname, mode, 0);
		KernelFS::numberOfOpenedFiles++;
		fileDescriptor->timesOpened++;
		ReleaseSRWLockExclusive(&srwLock);
		// acquire the file SRWLock in exclusive mode
		AcquireSRWLockExclusive(&(fileDescriptor->fileSRWLock));
		return file;
	}
	else {
		KernelFS::numberOfOpenedFiles++;
		fileDescriptor->timesOpened++;
		char fileDescriptorCluster[2048];
		File* file = nullptr;
		unsigned long fileSize = 0;
		switch (mode) {
		case 'r':
			ReleaseSRWLockExclusive(&srwLock);
			// acquire the file SRWLock in shared mode
			AcquireSRWLockShared(&(fileDescriptor->fileSRWLock));
			KernelFS::mountedPartition->readCluster(fileDescriptor->clusterNo, fileDescriptorCluster);
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 0]);
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 1]) << 8;
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 2]) << 16;
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 3]) << 24;
			return new File((std::string)fname, mode, fileSize);
		case 'w':
			ReleaseSRWLockExclusive(&srwLock);
			// acquire the file SRWLock in exclusive mode
			AcquireSRWLockExclusive(&(fileDescriptor->fileSRWLock));
			KernelFS::mountedPartition->readCluster(fileDescriptor->clusterNo, fileDescriptorCluster);
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 0]);
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 1]) << 8;
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 2]) << 16;
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 3]) << 24;
			file = new File((std::string)fname, mode, fileSize);
			file->truncate();
			return file;
		case 'a':
			ReleaseSRWLockExclusive(&srwLock);
			// acquire the file SRWLock in exclusive mode
			AcquireSRWLockExclusive(&(fileDescriptor->fileSRWLock));
			KernelFS::mountedPartition->readCluster(fileDescriptor->clusterNo, fileDescriptorCluster);
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 0]);
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 1]) << 8;
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 2]) << 16;
			fileSize |= ((unsigned char)fileDescriptorCluster[fileDescriptor->entryStart + KernelFS::FILE_SIZE_OFFSET + 3]) << 24;
			file = new File((std::string)fname, mode, fileSize);
			file->seek(file->getFileSize());
			return file;
		default:
			ReleaseSRWLockExclusive(&srwLock);
			return nullptr;
		}
	}
}

void KernelFS::deallocateCluster(ClusterNo clusterNo) {
	int clustNo = clusterNo / (ClusterSize * CHAR_SIZE_IN_BITS);
	int bytNo = (clusterNo % (ClusterSize * CHAR_SIZE_IN_BITS)) / CHAR_SIZE_IN_BITS;
	int bitNo = (clusterNo % (ClusterSize * CHAR_SIZE_IN_BITS)) % CHAR_SIZE_IN_BITS;
	char bitVectorCluster[2048];
	KernelFS::mountedPartition->readCluster(0 + clustNo, bitVectorCluster);
	bitVectorCluster[bytNo] |= (1 << bitNo);
	KernelFS::mountedPartition->writeCluster(0 + clustNo, bitVectorCluster);
}

char KernelFS::deleteFile(char* fname) {
	if (fname == nullptr) return 0;
	AcquireSRWLockExclusive(&srwLock);
	if (KernelFS::mountedPartition == nullptr || KernelFS::formattedPartitions[KernelFS::mountedPartition] == false) {
		ReleaseSRWLockExclusive(&srwLock);
		return 0;
	}
	FileDesc* fd = nullptr;
	if ((fd = KernelFS::getFileDescriptor(fname)) == nullptr) {
		ReleaseSRWLockExclusive(&srwLock);
		return 0; // file not found
	}
	if (fd->timesOpened > 0) {
		ReleaseSRWLockExclusive(&srwLock);
		return 0; // file is currently opened
	}
	char fileDescriptorCluster[2048];
	char fileLvl1IndexCluster[2048];
	char fileLvl2IndexCluster[2048];
	KernelFS::mountedPartition->readCluster(fd->clusterNo, fileDescriptorCluster);
	ClusterNo fileLvl1IndexClusterNo = 0;
	fileLvl1IndexClusterNo |= ((unsigned char)fileDescriptorCluster[fd->entryStart + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 0]);
	fileLvl1IndexClusterNo |= ((unsigned char)fileDescriptorCluster[fd->entryStart + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 1]) << 8;
	fileLvl1IndexClusterNo |= ((unsigned char)fileDescriptorCluster[fd->entryStart + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 2]) << 16;
	fileLvl1IndexClusterNo |= ((unsigned char)fileDescriptorCluster[fd->entryStart + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 3]) << 24;
	KernelFS::mountedPartition->readCluster(fileLvl1IndexClusterNo, fileLvl1IndexCluster);
	for (int lvl1Entry = 0; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		ClusterNo fileLvl2IndexClusterNo = 0;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 0]);
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 1]) << 8;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 2]) << 16;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 3]) << 24;
		if (fileLvl2IndexClusterNo == 0) continue; // no level 2 index cluster
		KernelFS::mountedPartition->readCluster(fileLvl2IndexClusterNo, fileLvl2IndexCluster);
		for (int lvl2Entry = 0; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			ClusterNo fileDataClusterNo = 0;
			fileDataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 0]);
			fileDataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 1]) << 8;
			fileDataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 2]) << 16;
			fileDataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 3]) << 24;
			if (fileDataClusterNo == 0) continue; // no data cluster
			KernelFS::deallocateCluster(fileDataClusterNo);
			// KernelFS::mountedPartition->writeCluster(fileDataClusterNo, emptyCluster);
		}
		KernelFS::deallocateCluster(fileLvl2IndexClusterNo);
		// KernelFS::mountedPartition->writeCluster(fileLvl2IndexClusterNo, emptyCluster);
	}
	KernelFS::deallocateCluster(fileLvl1IndexClusterNo);
	// KernelFS::mountedPartition->writeCluster(fileLvl1IndexClusterNo, emptyCluster);
	// free the file descriptor spot
	for (int offset = 0; offset < KernelFS::FILEDESC_ENTRY_SIZE_IN_BYTES; offset++)
		fileDescriptorCluster[fd->entryStart + offset] = 0x00;
	KernelFS::mountedPartition->writeCluster(fd->clusterNo, fileDescriptorCluster);
	// file descriptor spot freed
	// remove the file descriptor from the files map
	delete KernelFS::files.find((std::string)fname)->second;
	KernelFS::files.erase((std::string)fname);
	ReleaseSRWLockExclusive(&srwLock);
	return 1;
}