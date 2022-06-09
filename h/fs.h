#ifndef _FS_H_
#define _FS_H_
typedef long FileCnt;
typedef unsigned long BytesCnt;

const unsigned int FNAMELEN = 8; // maximum file name length (in characters)
const unsigned int FEXTLEN = 3; // maximum file extension length (in characters)

class KernelFS;
class Partition;
class File;

class FS {
public:
	
	~FS();

	/*
	Description: mounts the partition; the file system allows only one mounted partition at a time;
		therefore, whoever tries mounting a partition when there already is a mounted one will get blocked
	Return value(s): 
		- 1 if mounting was successfull, 
		- 0 otherwise
	Potential errors:
		- partition is a null pointer
	*/
	static char mount(Partition* partition);
	/*
	Description: unmounts the partition; whoever calls unmount will get blocked untill all files from the mounted partition are closed
	Reutrn value(s): 
		- 1 if unmounting was successfull, 
		- 0 otherwise
	Potential errors:
		- there is no mounted partition yet
	*/
	static char unmount();

	/*
	Desription: formats the MOUNTED partition by initializing all of the data structures required for the file system to work
	Return value(s):
		- 1 if formatting was successfull,
		- 0 otherwise
	Potential errors:
		- there is no mounted partition yet
		- the mounted partition is already formatted???
		- out of memory exception (when forming a buffer to initialize the bit-vector and the root directory)
	*/
	static char format();

	/*
	Description: returns the number of files on the mounted partition
	Return value(s): 
		- -1 in case of an error,
		- the number of files on the mounted partition otherwise
	Potential errors:
		- there is no mounted partition yet
		- the mounted partition is not formatted yet
		- out of memory exception (when forming a buffer to read entries inside of the root directory)
		- readCluster method from part.h returning error
	*/
	static FileCnt readRootDir();

	/*
	Description: checks whether or not the file, with the given fname (ABSOLUTE PATH) as an argument, exists inside of the root directory
	Return value(s): 
		- 0 if the file does not exist
		- 1 if the files exists
	Potential errors:
		- fname is a null pointer
		- there is no mounted partition yet
		- the mounted partition is not formatted yet
	*/
	static char doesExist(char* fname);

	/*
	Description: opens a file with the given fname (ABSOLUTE PATH) as an argument in the given mode;
		if the file has already been opened exclusively (write mode), the running thread is blocked untill it is safe to open the file in the given mode
	Return value(s):
		- a pointer towards a File object which represents the open file
		- null in case of an error
	Potential errors:
		- fname is a null pointer
		- opening a non-existing file in 'r'/'a' mode 
	Other: Modes:
				- 'r' - file is open in read-only mode; if the file does not exist an error is returned
				- 'w' - file is open in both read and write mode; if the file exists, its content is DELETED, otherwise - creates new file
				- 'a' - file is open either in read or write mode; cursor is set to the end of file; file has to exist, otherwise - an error is returned 
	*/
	static File* open(char* fname, char mode);
	/*
	Description: deletes a file with the given fname (ABSOLUTE PATH) as an argument, ONLY if the file is not open
	Return value(s):
		- 0 in case of an error
		- 1, otherwise
	Potential errors:
		- fname is a null pointer
		- file with the given fname does not exist
		- file is open
	*/
	static char deleteFile(char* fname);

protected:
	FS();
	static KernelFS *myImpl;
};

#endif // _FS_H_

