#ifndef _KERNELFS_H_
#define _KERNELFS_H_

#include "fs.h"
#include "part.h"
#include <windows.h>
#include <iostream>
#include <string>
#include <unordered_map>
#include <synchapi.h>

const unsigned CHAR_SIZE_IN_BITS = 8;

class File;
class FileDesc;
class ClusterCache;

class KernelFS {
public:

	static char mount(Partition* partition);
	static char unmount();

	static char format();

	static FileCnt readRootDir();

	static char doesExist(char* fname);

	static File* open(char* fname, char mode);
	static char deleteFile(char* fname);

	static const unsigned int
		LVL1_ENTRY_SIZE_IN_BYTES,
		LVL2_ENTRY_SIZE_IN_BYTES,
		FILEDESC_ENTRY_SIZE_IN_BYTES;
	static const unsigned int
		FILE_NAME_OFFSET,
		FILE_EXTENSION_OFFSET,
		LVL1_INDEX_CLUSTER_NUMBER_OFFSET,
		NOT_IN_USE_BYTE_OFFSET,
		FILE_SIZE_OFFSET;

private:

	friend class KernelFile;
	friend class FS;
	friend class ClusterCache;

	KernelFS();

	// used to block the running thread if it tries to mount/unmount/format when it cannot do so (type: SemaphoreObject(s) from win32 API)
	static HANDLE ok_to_mount, ok_to_unmount, ok_to_format; 
	// used for mutual exclusion when accessing the attributes, but allows reader parallelism (type: Slim Reader/Writer Lock from synchapi.h)
	static SRWLOCK srwLock;


	static int waitingToUnMount, waitingToFormat;
	static long numberOfOpenedFiles;
	static int numOfClusters;
	static int bitVectorSizeInClusters;
	static int rootLvl1IndexClusterNo;

	// nullptr - no partition is yet mounted; != nullptr - pointer towards the mounted partition
	static Partition* mountedPartition;
	// mapping a partition pointer into a bool value that tells whether or not the partition has been formatted
	static std::unordered_map<Partition*, bool> formattedPartitions;
	/* mapping a file name into an FileDesc object which stores general information about the file:
			- file descriptor, which is an entry into the root directory corresponding to the file
			- ClusterNo and entryStart which define the location of the file descriptor above on the mounted partition
			- number of threads who currently have the file opened
	*/
	static std::unordered_map<std::string, FileDesc*> files;

	/*
	Description:
		properly initializes bit vector: allocates the amount of clusters needed for the bit vector and initializes them as not free in it
	*/
	static void initializeBitVector();

	/*
	Description: 
		returns a file descriptor which holds information about a file which name is given as an argument, if the file exists
	Return value(s): 
		- nullptr, if the file is not found
		- FileDesc* (!= nullptr), if the file is found
	*/
	static FileDesc* getFileDescriptor(char* fname);
	/*
	Description: 
		formats the given file name (through 1st argument) into a file name (returns through 2nd argument) and a file extension
			(returns through 3rd argument)
	Return value(s):
		- 0, if the formatting was unsuccessful
		- 1, otherwise
	Potential errors:
		- given file name has wrong format
	*/
	static char format(char* fname, char* fileName, char* fileExtension);

	/*
	Description:
		allocates a new cluster, evidenting it inside of the bit vector
	Return value(s):
		- cluster number of the allocated cluster, if allocation has been successfull
		- 0 otherwise
	Potential errors:
		- no free clusters left
	*/
	static ClusterNo allocateCluster();

	/*
	Description:
		allocates a new file descriptor and inserts informations about the new file into it
	Return value(s):
		- 0, if the descriptor has been allocated successfully
		- 1, otherwise
	Potential errors: 
		- no file descriptors available left / no clusters for file's lvl 1 index cluster left / ...
	*/
	static char allocateFileDescriptor(std::string fnameUnformatted, char* fileName, char* fileExtension);

	/*
	Description:
		deallocates a cluster which number is given through the parameter, evidenting it as free inside of the bit vector
	*/
	static void deallocateCluster(ClusterNo clusterNo);

};

#endif // _KERNELFS_H_