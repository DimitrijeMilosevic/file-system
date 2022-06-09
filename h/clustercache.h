#ifndef _CLUSTERCACHE_H_
#define _CLUSTERCACHE_H_

#include "part.h"
#include <Windows.h>
#include <synchapi.h>

const unsigned int CACHE_SIZE = 128;

class KernelFile;

class ClusterCache {
public:

	ClusterCache();
	void readCluster(ClusterNo clusterNo, char* buffer);
	void writeCluster(ClusterNo clusterNo, char* buffer);

private:

	friend class KernelFile;

	/*
	Description:
		checks wether or not cluster with the given cluster number is cached
	Return value(s):
		- >=0 (entry number inside of the cache), if is
		- -1 if it isn't
	*/
	int exists(ClusterNo clusterNo);
	/*
	Description:
		returns next entry number for data to be stored;
		if no non-valid entries are found, one entry will be freed up (and written back if needed), and its number returned;
	*/
	int getNextEntry();
	// invalidates an entry which holds a cached cluster with the given cluster number
	void invalidate(ClusterNo);
	// writes back all of the clusters modified by a thread who opened the file in 'w' mode - used for threads when closing files opened in 'w'/'a' mode
	void writeBack();

	bool valid[CACHE_SIZE], dirty[CACHE_SIZE];
	ClusterNo tag[CACHE_SIZE];
	char data[CACHE_SIZE][ClusterSize];
	SRWLOCK cacheSRWLock;


};

#endif