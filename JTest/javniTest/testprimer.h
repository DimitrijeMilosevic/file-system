#pragma once

#include"fs.h"
#include"file.h"
#include"part.h"

#include<iostream>
#include<fstream>
#include<cstdlib>
#include<windows.h>
#include<ctime>

#define signal(x) ReleaseSemaphore(x,1,NULL)
#define wait(x) WaitForSingleObject(x,INFINITE)

using namespace std;

extern HANDLE nit1,nit2;
extern DWORD ThreadID;

extern HANDLE mutex;
extern HANDLE semMain;
extern HANDLE sem12;
extern HANDLE sem21;

extern Partition *partition;

extern char *ulazBuffer;
extern int ulazSize;

DWORD WINAPI nit1run();
DWORD WINAPI nit2run();