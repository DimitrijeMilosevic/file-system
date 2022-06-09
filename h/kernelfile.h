#ifndef _KERNELFILE_H_
#define _KERNELFILE_H_

#include "file.h"
#include "part.h"

class FileDesc;

class KernelFile {
public:

	KernelFile(std::string fname, char mode, BytesCnt fileSize);
	~KernelFile();
	char write(BytesCnt, char* buffer);
	BytesCnt read(BytesCnt, char* buffer);
	char seek(BytesCnt);
	BytesCnt filePos();
	char eof();
	BytesCnt getFileSize();
	char truncate();

private:

	// allocates cluster atomically (call of KernelFS::allocateCluster() is surrounded by acquiring/releasing KernelFS::srwLock)
	ClusterNo allocateClusterAtomic();
	// deallocates given cluster atomically (call of KernelFS::deallocateCluster() is surrounded by acquiring/releasing KernelFS::srwLock)
	void deallocateClusterAtomic(ClusterNo);
	
	// checks whether or not the given file's level 2 index cluster can be deallocated
	bool okToDeallocate(char* fileLvl2IndexCluster);

	// updates file size 
	void updateFileSize(FileDesc* fileDesc);


	std::string fname;
	char mode;
	ClusterNo fileLvl1IndexClusterNo;
	BytesCnt fileSize;
	BytesCnt cursor;

};

#endif // _KERNELFILE_H_
