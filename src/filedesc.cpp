#include "filedesc.h"
#include "clustercache.h"

FileDesc::FileDesc(ClusterNo clusterNo, unsigned int entryStart) {
	this->clusterNo = clusterNo;
	this->entryStart = entryStart;
	this->timesOpened = 0;
	cache = new ClusterCache();
	fileSRWLock = SRWLOCK_INIT;
}

FileDesc::~FileDesc() {
	delete cache;
}