#include "kernelfile.h"
#include "KernelFS.h"
#include "filedesc.h"
#include "clustercache.h"

KernelFile::KernelFile(std::string fname, char mode, BytesCnt fileSize) : cursor(0) {
	this->fname = fname;
	this->mode = mode;
	this->fileSize = fileSize;
	char fileDescriptorCluster[2048];
	KernelFS::mountedPartition->readCluster(KernelFS::files[fname]->clusterNo, fileDescriptorCluster);
	fileLvl1IndexClusterNo = 0;
	fileLvl1IndexClusterNo |= ((unsigned char)fileDescriptorCluster[KernelFS::files[fname]->entryStart + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 0]);
	fileLvl1IndexClusterNo |= ((unsigned char)fileDescriptorCluster[KernelFS::files[fname]->entryStart + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 1]) << 8;
	fileLvl1IndexClusterNo |= ((unsigned char)fileDescriptorCluster[KernelFS::files[fname]->entryStart + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 2]) << 16;
	fileLvl1IndexClusterNo |= ((unsigned char)fileDescriptorCluster[KernelFS::files[fname]->entryStart + KernelFS::LVL1_INDEX_CLUSTER_NUMBER_OFFSET + 3]) << 24;
}

KernelFile::~KernelFile() {
	AcquireSRWLockExclusive(&(KernelFS::srwLock));
	KernelFS::files[fname]->timesOpened--;
	KernelFS::numberOfOpenedFiles--;
	KernelFile::updateFileSize(KernelFS::files[fname]);
	// unblock threads waiting to unmount/format the mounted partition... to be implemented
	if (KernelFS::numberOfOpenedFiles == 0 && KernelFS::waitingToUnMount > 0)
		ReleaseSemaphore(
			KernelFS::ok_to_unmount, // handle to semaphore
			KernelFS::waitingToUnMount, // increase count by 1
			NULL // not interested in previous count
		);
	else if (KernelFS::numberOfOpenedFiles == 0 && KernelFS::waitingToFormat > 0)
		ReleaseSemaphore(
			KernelFS::ok_to_format,
			KernelFS::waitingToFormat,
			NULL
		);
	if (mode == 'w' || mode == 'a')
		KernelFS::files[fname]->cache->writeBack();
	ReleaseSRWLockExclusive(&(KernelFS::srwLock));
	if (mode == 'r')
		ReleaseSRWLockShared(&(KernelFS::files[fname]->fileSRWLock));
	else if (mode == 'w' || mode == 'a')
		ReleaseSRWLockExclusive(&(KernelFS::files[fname]->fileSRWLock));
}

ClusterNo KernelFile::allocateClusterAtomic() {
	AcquireSRWLockExclusive(&(KernelFS::srwLock));
	ClusterNo allocatedClusterNo = KernelFS::allocateCluster();
	ReleaseSRWLockExclusive(&(KernelFS::srwLock));
	return allocatedClusterNo;
}

void KernelFile::updateFileSize(FileDesc* fileDesc) {
	char fileDescriptorCluster[2048];
	KernelFS::mountedPartition->readCluster(fileDesc->clusterNo, fileDescriptorCluster);
	fileDescriptorCluster[fileDesc->entryStart + KernelFS::FILE_SIZE_OFFSET + 0] = fileSize & 0xffUL;
	fileDescriptorCluster[fileDesc->entryStart + KernelFS::FILE_SIZE_OFFSET + 1] = (fileSize >> 8) & 0xffUL;
	fileDescriptorCluster[fileDesc->entryStart + KernelFS::FILE_SIZE_OFFSET + 2] = (fileSize >> 16) & 0xffUL;
	fileDescriptorCluster[fileDesc->entryStart + KernelFS::FILE_SIZE_OFFSET + 3] = (fileSize >> 24) & 0xffUL;
	KernelFS::mountedPartition->writeCluster(fileDesc->clusterNo, fileDescriptorCluster);
}

char KernelFile::write(BytesCnt bytesCnt, char* buffer) {
	if (mode == 'r') return 0; // if the file is opened in read-only mode, no writing is allowed
	if (bytesCnt == 0) return 0; // nothing to write
	char emptyCluster[2048];
	for (int i = 0; i < 2048; i++)
		emptyCluster[i] = 0x00;
	char fileLvl1IndexCluster[2048];
	KernelFS::mountedPartition->readCluster(fileLvl1IndexClusterNo, fileLvl1IndexCluster);
	int startingLvl1EntryNo = cursor / (512 * ClusterSize); // one level 2 entry can have 512 data clusters (each with 2048B in it)
	int startingLvl2EntryNo = (cursor % (512 * ClusterSize)) / ClusterSize;
	int startingByteNo = (cursor % (512 * ClusterSize)) % ClusterSize;
	BytesCnt nextByteToWrite = 0;
	for (int lvl1Entry = startingLvl1EntryNo * KernelFS::LVL1_ENTRY_SIZE_IN_BYTES; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		ClusterNo fileLvl2IndexClusterNo = 0;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 0]);
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 1]) << 8;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 2]) << 16;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 3]) << 24;
		if (fileLvl2IndexClusterNo == 0) {
			fileLvl2IndexClusterNo = KernelFile::allocateClusterAtomic();
			if (fileLvl2IndexClusterNo == 0 || fileLvl2IndexClusterNo > (KernelFS::numOfClusters - 1)) return 0; // no free cluster found
			KernelFS::mountedPartition->writeCluster(fileLvl2IndexClusterNo, emptyCluster);
			fileLvl1IndexCluster[lvl1Entry + 0] = fileLvl2IndexClusterNo & 0xffUL;
			fileLvl1IndexCluster[lvl1Entry + 1] = (fileLvl2IndexClusterNo >> 8) & 0xffUL;
			fileLvl1IndexCluster[lvl1Entry + 2] = (fileLvl2IndexClusterNo >> 16) & 0xffUL;
			fileLvl1IndexCluster[lvl1Entry + 3] = (fileLvl2IndexClusterNo >> 24) & 0xffUL;
			startingLvl2EntryNo = startingByteNo = 0; // if a new cluster has been allocated writing should start from beggining
		}
		char fileLvl2IndexCluster[2048];
		KernelFS::mountedPartition->readCluster(fileLvl2IndexClusterNo, fileLvl2IndexCluster);
		for (int lvl2Entry = startingLvl2EntryNo * KernelFS::LVL2_ENTRY_SIZE_IN_BYTES; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			ClusterNo dataClusterNo = 0;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 0]);
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 1]) << 8;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 2]) << 16;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 3]) << 24;
			if (dataClusterNo == 0) {
				dataClusterNo = KernelFile::allocateClusterAtomic();
				if (dataClusterNo == 0 || dataClusterNo > (KernelFS::numOfClusters - 1)) return 0; // no free cluster found
				KernelFS::files[fname]->cache->writeCluster(dataClusterNo, emptyCluster);
				fileLvl2IndexCluster[lvl2Entry + 0] = dataClusterNo & 0xffUL;
				fileLvl2IndexCluster[lvl2Entry + 1] = (dataClusterNo >> 8) & 0xffUL;
				fileLvl2IndexCluster[lvl2Entry + 2] = (dataClusterNo >> 16) & 0xffUL;
				fileLvl2IndexCluster[lvl2Entry + 3] = (dataClusterNo >> 24) & 0xffUL;
				startingByteNo = 0;
			}
			char dataCluster[2048];
			KernelFS::files[fname]->cache->readCluster(dataClusterNo, dataCluster);
			int numOfBytesToWrite = ((ClusterSize - startingByteNo) > (bytesCnt - nextByteToWrite)) ? (bytesCnt - nextByteToWrite)
				: (ClusterSize - startingByteNo);
			for (int byteNo = 0; byteNo < numOfBytesToWrite; byteNo++)
				dataCluster[startingByteNo + byteNo] = buffer[nextByteToWrite++];
			KernelFS::files[fname]->cache->writeCluster(dataClusterNo, dataCluster);
			if (nextByteToWrite == bytesCnt) { // writing finished successfully
				fileSize += bytesCnt;
				cursor += bytesCnt;
				KernelFS::mountedPartition->writeCluster(fileLvl2IndexClusterNo, fileLvl2IndexCluster);
				KernelFS::mountedPartition->writeCluster(fileLvl1IndexClusterNo, fileLvl1IndexCluster);
				return 1;
			}
			if (startingByteNo != 0)
				startingByteNo = 0;
		}
		if (startingLvl2EntryNo != 0) 
			startingLvl2EntryNo = 0;
		KernelFS::mountedPartition->writeCluster(fileLvl2IndexClusterNo, fileLvl2IndexCluster);
	}
	return 0;
}

BytesCnt KernelFile::read(BytesCnt bytesCnt, char* buffer) {
	if (bytesCnt == 0) return 0; // nothing to read
	if (cursor == fileSize) return 0; // cursor is at the eof
	if (bytesCnt > (fileSize - cursor)) // up to how many bytes can be read 
		bytesCnt = fileSize - cursor;
	char fileLvl1IndexCluster[2048];
	KernelFS::mountedPartition->readCluster(fileLvl1IndexClusterNo, fileLvl1IndexCluster);
	int startingLvl1EntryNo = cursor / (512 * ClusterSize); // one level 2 entry can have 512 data clusters (each with 2048B in it)
	int startingLvl2EntryNo = (cursor % (512 * ClusterSize)) / ClusterSize;
	int startingByteNo = (cursor % (512 * ClusterSize)) % ClusterSize;
	BytesCnt numOfBytesRead = 0;
	for (int lvl1Entry = startingLvl1EntryNo * KernelFS::LVL1_ENTRY_SIZE_IN_BYTES; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		ClusterNo fileLvl2IndexClusterNo = 0;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 0]);
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 1]) << 8;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 2]) << 16;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 3]) << 24;
		char fileLvl2IndexCluster[2048];
		KernelFS::mountedPartition->readCluster(fileLvl2IndexClusterNo, fileLvl2IndexCluster);
		for (int lvl2Entry = startingLvl2EntryNo * KernelFS::LVL2_ENTRY_SIZE_IN_BYTES; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			ClusterNo dataClusterNo = 0;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 0]);
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 1]) << 8;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 2]) << 16;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 3]) << 24;
			char dataCluster[2048];
			KernelFS::files[fname]->cache->readCluster(dataClusterNo, dataCluster);
			int numOfBytesToRead = ((ClusterSize - startingByteNo) > (bytesCnt - numOfBytesRead)) ? (bytesCnt - numOfBytesRead)
				: (ClusterSize - startingByteNo);
			for (int byteNo = 0; byteNo < numOfBytesToRead; byteNo++)
				buffer[numOfBytesRead++] = dataCluster[startingByteNo + byteNo];
			if (numOfBytesRead == bytesCnt) { // reading done 
				cursor += numOfBytesRead;
				return numOfBytesRead;
			}
			if (startingByteNo != 0)
				startingByteNo = 0;
		}
		if (startingLvl2EntryNo != 0)
			startingLvl2EntryNo = 0;
	}
	return 0;
}

char KernelFile::seek(BytesCnt position) {
	if (position > fileSize) // seeking not possible
		return 0;
	else {
		cursor = position;
		return 1;
	}
}

BytesCnt KernelFile::filePos() {
	return cursor;
}

char KernelFile::eof() {
	if (cursor == fileSize)
		return 2;
	else
		return 0;
}

BytesCnt KernelFile::getFileSize() {
	return fileSize;
}

void KernelFile::deallocateClusterAtomic(ClusterNo clusterNo) {
	AcquireSRWLockExclusive(&(KernelFS::srwLock));
	KernelFS::deallocateCluster(clusterNo);
	ReleaseSRWLockExclusive(&(KernelFS::srwLock));
}

bool KernelFile::okToDeallocate(char* fileLvl2IndexCluster) {
	bool okToDeallocate = true;
	for (int lvl2Entry = 0; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
		ClusterNo dataClusterNo = 0;
		dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 0]);
		dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 1]) << 8;
		dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 2]) << 16;
		dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 3]) << 24;
		if (dataClusterNo != 0) {
			okToDeallocate = false;
			break;
		}
	}
	return okToDeallocate;
}

char KernelFile::truncate() {
	if (mode == 'r') return 0; // if the file is opened in read-only mode, truncation is not allowed
	if (cursor == fileSize) return 0; // cursor is at the eof
	char fileLvl1IndexCluster[2048];
	KernelFS::mountedPartition->readCluster(fileLvl1IndexClusterNo, fileLvl1IndexCluster);
	int startingLvl1EntryNo = cursor / (512 * ClusterSize); // one level 2 entry can have 512 data clusters (each with 2048B in it)
	int startingLvl2EntryNo = (cursor % (512 * ClusterSize)) / ClusterSize;
	int startingByteNo = (cursor % (512 * ClusterSize)) % ClusterSize;
	long numOfBytesToTruncate = fileSize - cursor;
	long numOfBytesLeftToTruncate = numOfBytesToTruncate;
	for (int lvl1Entry = startingLvl1EntryNo * KernelFS::LVL1_ENTRY_SIZE_IN_BYTES; lvl1Entry < 2048; lvl1Entry += KernelFS::LVL1_ENTRY_SIZE_IN_BYTES) {
		ClusterNo fileLvl2IndexClusterNo = 0;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 0]);
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 1]) << 8;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 2]) << 16;
		fileLvl2IndexClusterNo |= ((unsigned char)fileLvl1IndexCluster[lvl1Entry + 3]) << 24;
		char fileLvl2IndexCluster[2048];
		KernelFS::mountedPartition->readCluster(fileLvl2IndexClusterNo, fileLvl2IndexCluster);
		for (int lvl2Entry = startingLvl2EntryNo * KernelFS::LVL2_ENTRY_SIZE_IN_BYTES; lvl2Entry < 2048; lvl2Entry += KernelFS::LVL2_ENTRY_SIZE_IN_BYTES) {
			ClusterNo dataClusterNo = 0;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 0]);
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 1]) << 8;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 2]) << 16;
			dataClusterNo |= ((unsigned char)fileLvl2IndexCluster[lvl2Entry + 3]) << 24;
			if (startingByteNo == 0) { // whole data cluster needs to be deallocated
				KernelFile::deallocateClusterAtomic(dataClusterNo);
				KernelFS::files[fname]->cache->invalidate(dataClusterNo);
				// update file's level 2 index cluster
				fileLvl2IndexCluster[lvl2Entry + 0] = 0x00;
				fileLvl2IndexCluster[lvl2Entry + 1] = 0x00;
				fileLvl2IndexCluster[lvl2Entry + 2] = 0x00;
				fileLvl2IndexCluster[lvl2Entry + 3] = 0x00;
				numOfBytesLeftToTruncate -= ClusterSize;
			}
			else 
				numOfBytesLeftToTruncate -= (ClusterSize - startingByteNo);
			if (numOfBytesLeftToTruncate <= 0) { // truncation completed
				if (okToDeallocate(fileLvl2IndexCluster) == false)
					KernelFS::mountedPartition->writeCluster(fileLvl2IndexClusterNo, fileLvl2IndexCluster);
				else {
					KernelFile::deallocateClusterAtomic(fileLvl2IndexClusterNo);
					// update file level 1 index cluster
					fileLvl1IndexCluster[lvl1Entry + 0] = 0x00;
					fileLvl1IndexCluster[lvl1Entry + 1] = 0x00;
					fileLvl1IndexCluster[lvl1Entry + 2] = 0x00;
					fileLvl1IndexCluster[lvl1Entry + 3] = 0x00;
				}
				KernelFS::mountedPartition->writeCluster(fileLvl1IndexClusterNo, fileLvl1IndexCluster);
				fileSize -= numOfBytesToTruncate;
				return 1;
			}
			if (startingByteNo != 0)
				startingByteNo = 0;
		}
		if (okToDeallocate(fileLvl2IndexCluster) == false)
			KernelFS::mountedPartition->writeCluster(fileLvl2IndexClusterNo, fileLvl2IndexCluster);
		else {
			KernelFile::deallocateClusterAtomic(fileLvl2IndexClusterNo);
			// update file level 1 index cluster
			fileLvl1IndexCluster[lvl1Entry + 0] = 0x00;
			fileLvl1IndexCluster[lvl1Entry + 1] = 0x00;
			fileLvl1IndexCluster[lvl1Entry + 2] = 0x00;
			fileLvl1IndexCluster[lvl1Entry + 3] = 0x00;
		}
		if (startingLvl2EntryNo != 0)
			startingLvl2EntryNo = 0;
	}
	return 0;
}