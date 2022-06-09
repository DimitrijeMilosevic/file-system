#include "clustercache.h"
#include "KernelFS.h"
#include <cstdlib>

ClusterCache::ClusterCache() {
	for (int i = 0; i < CACHE_SIZE; i++) {
		valid[i] = dirty[i] = tag[i] = 0;
		for (int j = 0; j < ClusterSize; j++)
			data[i][j] = 0;
	}
	cacheSRWLock = SRWLOCK_INIT;
}

int ClusterCache::exists(ClusterNo clusterNo) {
	for (int entryNo = 0; entryNo < CACHE_SIZE; entryNo++)
		if (valid[entryNo] == 1 && tag[entryNo] == clusterNo)
			return entryNo;
	return -1; // cluster isn't cached
}

int ClusterCache::getNextEntry() {
	// find an invalid entry
	int entryNo;
	for (entryNo = 0; entryNo < CACHE_SIZE; entryNo++)
		if (valid[entryNo] == 0)
			return entryNo;
	// no invalid entry found - all entries are valid
	// find a non-dirty valid entry
	for (entryNo = 0; entryNo < CACHE_SIZE; entryNo++)
		if (dirty[entryNo] == 0)
			return entryNo;
	// all entries are dirty and invalid - write back a random one - unreachable code when it comes to threads who opened file in 'r' mode
	entryNo = (int)(rand() % CACHE_SIZE);
	KernelFS::mountedPartition->writeCluster(tag[entryNo], data[entryNo]);
	return entryNo;
}

void ClusterCache::readCluster(ClusterNo clusterNo, char* buffer) {
	AcquireSRWLockShared(&cacheSRWLock);
	int entryNo;
	if ((entryNo = exists(clusterNo)) != -1) { // cluster found
		for (int i = 0; i < ClusterSize; i++)
			buffer[i] = data[entryNo][i];
		ReleaseSRWLockShared(&cacheSRWLock);
	}
	else {
		ReleaseSRWLockShared(&cacheSRWLock);
		AcquireSRWLockExclusive(&cacheSRWLock);
		entryNo = getNextEntry();
		valid[entryNo] = 1;
		dirty[entryNo] = 0;
		tag[entryNo] = clusterNo;
		KernelFS::mountedPartition->readCluster(clusterNo, buffer);
		for (int i = 0; i < ClusterSize; i++)
			data[entryNo][i] = buffer[i];
		ReleaseSRWLockExclusive(&cacheSRWLock);
	}
}

void ClusterCache::writeCluster(ClusterNo clusterNo, char* buffer) {
	int entryNo;
	if ((entryNo = exists(clusterNo)) == -1) {
		entryNo = getNextEntry();
		valid[entryNo] = 1;
		tag[entryNo] = clusterNo;
	}
	for (int i = 0; i < ClusterSize; i++)
		data[entryNo][i] = buffer[i];
	if (dirty[entryNo] == 0)
		dirty[entryNo] = 1;
}

void ClusterCache::invalidate(ClusterNo clusterNo) {
	int entryNo;
	if ((entryNo = exists(clusterNo)) != -1) {
		valid[entryNo] = dirty[entryNo] = tag[entryNo] = 0;
		for (int i = 0; i < ClusterSize; i++)
			data[entryNo][i] = 0;
	}
}

void ClusterCache::writeBack() {
	for (int entryNo = 0; entryNo < CACHE_SIZE; entryNo++) {
		if (valid[entryNo] == 0 || dirty[entryNo] == 0) continue;
		KernelFS::mountedPartition->writeCluster(tag[entryNo], data[entryNo]);
		dirty[entryNo] = 0;
	}
}