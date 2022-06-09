#ifndef _FILEDESC_H_
#define _FILEDESC_H_

#include "part.h"
#include <Windows.h>
#include <synchapi.h>

class ClusterCache;

class FileDesc {
public:

	FileDesc(ClusterNo clusterNo, unsigned int entryStart);
	~FileDesc();

	friend class KernelFS;
	friend class KernelFile;

private:

	ClusterNo clusterNo; // a cluster number in which the file descriptor is stored
	unsigned int entryStart; // the entry's starting byte inside of the file descriptor cluster
	unsigned int timesOpened; // how many times is the file opened at any given moment
	ClusterCache* cache;
	SRWLOCK fileSRWLock; // an SRWLock for the file

};

#endif // _FILEDESC_H_