#ifndef _FILE_H_
#define _FILE_H_

#include "fs.h"
#include <string>
class KernelFile;

class File {
public:

	/*
	Description: 
		destructor is used as a method to close the file;
		closing the file means freeing it up for other threads to use it;
		depending on how the file was opened, freeing a file has different releases of the file's SRWLock 
	*/
	~File();
	
	/*
	Description: 
		writes BytesCnt bytes to the file, starting from the current position of the file, expanding file's size if needed
	Return value(s):
		- 0, in case of an error
		- 1, if writing was successfull
	Potential errors:
		- file was opened in 'r' mode
		- allocating new data clusters failed whilst expanding file
	*/
	char write(BytesCnt bytesCnt, char* buffer);

	/*
	Description: 
		reads BytesCnt bytes from the file starting from current position of the cursor; what is read is put inside of the buffer
		given as the second argument; 
	Notes: 
		- it is up to the user to provide a buffer which has enough space
		- number of read bytes can be lower than the BytesCnt (if the cursor has reached eof)
	Return value(s):
		- 0, in case of an error
		- >0 - number of read bytes
	Potential errors:
		- cursor was at the eof before the read(BytesCnt, char*) was called
	*/
	BytesCnt read(BytesCnt bytesCnt, char* buffer);
	
	/*
	Description:
		sets the cursor position to the BytesCnt byte
	Return value(s):
		- 0, in case of an error
		- 1, if seeking was successfull
	Potential errors:
		- BytesCnt byte is out of bounds (<0 || >fileSize)
	*/
	char seek(BytesCnt);

	/*
	Description + Return value:
		returns the BytesCnt which represents the number of byte of current position of the cursor
	*/
	BytesCnt filePos();

	/*
	Description: 
		checks whether or not the current position of the cursor equals eof
	Return value(s):
		- 0, if no
		- 1, in case of an error (?)
		- 2, if yes
	*/
	char eof();

	/*
	Description + Return value:
		returns BytesCnt which represents the file size in bytes
	*/
	BytesCnt getFileSize();

	/*
	Description:
		deletes file content starting from current position of the cursor untill the eof
	Return value(s):
		- 0, in case of an error (?)
		- 1, if the truncation was successfull
	*/
	char truncate();

private:

	friend class FS;
	friend class KernelFS;
	File(std::string fname, char mode, BytesCnt fileSize); // file object can only be created by opening a file
	KernelFile* myImpl;

};

#endif // _FILE_H_
