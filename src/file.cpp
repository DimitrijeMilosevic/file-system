#include "file.h"
#include "kernelfile.h"

File::File(std::string fname, char mode, BytesCnt fileSize) {
	myImpl = new KernelFile(fname, mode, fileSize);
}

File::~File() {
	delete myImpl;
}

char File::write(BytesCnt bytesCnt, char* buffer) {
	return myImpl->write(bytesCnt, buffer);
}

BytesCnt File::read(BytesCnt bytesCnt, char* buffer) {
	return myImpl->read(bytesCnt, buffer);
}

char File::seek(BytesCnt position) {
	return myImpl->seek(position);
}

BytesCnt File::filePos() {
	return myImpl->filePos();
}

char File::eof() {
	return myImpl->eof();
}

BytesCnt File::getFileSize() {
	return myImpl->getFileSize();
}

char File::truncate() {
	return myImpl->truncate();
}