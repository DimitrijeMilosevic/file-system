#include "fs.h"
#include "KernelFS.h"
FS::~FS() {}

char FS::mount(Partition* partition) {
	return KernelFS::mount(partition);
}

char FS::unmount() {
	return KernelFS::unmount();
}

char FS::format() {
	return KernelFS::format();
}

FileCnt FS::readRootDir() {
	return KernelFS::readRootDir();
}

char FS::doesExist(char* fname) {
	return KernelFS::doesExist(fname);
}

File* FS::open(char* fname, char mode) {
	return KernelFS::open(fname, mode);
}

char FS::deleteFile(char* fname) {
	return KernelFS::deleteFile(fname);
}

FS::FS() {}
