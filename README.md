# Overview

File system stores data permanently on a virtual hard disk, which needs to be partitioned before being used.

An interface for accessing partitions is provided and it contains methods for: creating an object representing a partition, fetching general info about a partition, reading data from a virtual hard disk cluster and writing data to a virtual hard disk cluster.

Interfaces for mounting and demounting of a partition and for working with files are implemented. 

File system may contain only one partition mounted at any time and only one directory (root directory) which stores all of the files - no subdirectories.

`kernelfs.cpp` implements an interface for general operations with partitions:
- fetching general info about the file system,
- mounting and demounting a partition,
- formatting a mounted partition - initializing required data structures,
- checking if a file exists, and
- deleting and opening (creating) a file.

`kernelfile.cpp` implements an interface for general operations with files:
- reading and writing an array of bytes of a given size, from the current position,
- fetching, checking and updating the current position,
- truncating a file, from the current position, and
- closing a file.

# Implementation Details

- A bit vector is used for registering free clusters.
- A two-level index-like structure is used for file allocation.
- A small cache of recently accessed indexes is used in order to lower the number of operations with the virtual disk (which are slow, compared to in-memory operations).
- All operations are thread-safe, which is ensured by using the *Win32 API* concurrent data structures (mutexes, multiple-readers-single-writer, etc.).
