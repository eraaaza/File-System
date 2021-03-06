/*************************************************************
* Class: CSC-415-01 Spring 2020
* Group Name: Team Alpha 1
* Name: Daniel Belmeur
* Student ID: 913525345
* Name: Brad Peraza
* Student ID: 916768260
* Name: Amily Wu
* Student ID:916929226
*
* Project: <Assignment 3–File System
*
* File: <CSC CSC 415 Assignment 3 Report>
*
*Description: Our File System
*************************************************************/


#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include "fsLow.h"

#define DIRFLAG_FILE 0x00000001 //1 if file 0 is directory
#define DIRFLAG_UNUSED 0x00000002
#define DIRFLAG_SPECIAL 0x00000004
#define DIRFILE_DIRECTORY 0x00000008
#define AVGDIRECTORYENTRIES 50
#define CONTIG 1
#define NOTCONTIG 0
#define FILEIDINCREMENT 17

#define FDOPENINUSE 0x00000001
#define FDOPENFREE  0x00000002
#define FDOPENMAX   50

#define FDOPENFORWRITE  0x00000010  //0000 0000 0000 0000 0000 0000 0001 0000
#define FDOPENFORREAD   0x00000020  //0000 0000 0000 0000 0000 0000 0010 0000

uint64_t actualDirEntries;

char * testString = "-----DEBUG-----";


typedef struct openFileEntry
  {
    int flags;
    uint64_t pointer;   //points to where in the file we already
    uint64_t size;     //size of the file in bytes
    uint64_t Id;
    uint64_t position;
    uint64_t blockStart;
    char * filebuffer;
  } openFileEntry, * openFileEntry_p;

openFileEntry * openFileList;   //this is an array of open files

typedef struct dirEntry
    {
      uint64_t Id;
      char name[128];
      uint64_t date;
      uint64_t location;    //LBA start
      uint64_t sizeInBytes; //blocks needed = (sizeInBytes + (blockSize - 1)) / blockSize;
      uint64_t flags;
    } dirEntry, * dirEntry_p;


//the global information about our filesystem is in the VCB

typedef struct vcb_t
      {
        uint64_t  volumeSize;
        uint64_t  blockSize;
        uint64_t  numberOfBlocks;

        uint64_t  freeBlockLocation;
        uint64_t  freeBlockBlocks;
        uint64_t  freeBlockLastAllocatedBit;
        uint64_t  freeBlockEndBlocksRemaining;
        uint64_t  freeBlockTotalFreeBlocks;
        uint64_t  currentVCB;
        char *    freeBuffer; //this is in memory only

        uint64_t  rootDirectoryStart;

        uint64_t  nextIdToIssue;

      } vcb_t, * vcb_p;

//here we need to initialize a volume control block

vcb_p  currentVCB_p;
void initVolumeControlBlock(uint64_t inputBlockSize, uint64_t inputVolumeSize)
  {
    printf("%ld\n", inputBlockSize);
    printf("%ld\n", inputVolumeSize);

    currentVCB_p =  malloc(sizeof(currentVCB_p));
    currentVCB_p->blockSize = inputBlockSize;
    currentVCB_p->volumeSize = inputVolumeSize;
    currentVCB_p->numberOfBlocks = inputVolumeSize / inputBlockSize;

    //printf("%s\n" ,testString);

    //DEBUG FOR blockSize
    printf("%s", "currentVCB_p blockSize = ");
        printf("%ld\n", currentVCB_p->blockSize);

    //DEBUG FOR volumeSize
    printf("%s", "currentVCB_p volumeSize = ");
        printf("%ld\n", currentVCB_p->volumeSize);

    //DEBUG FOR numberOfBlocks
    printf("%s", "currentVCB_p numberOfBlocks = ");
        printf("%ld\n", currentVCB_p->numberOfBlocks);

    //printf("%s\n" ,testString);


  }
uint64_t getNewFileID()
  {
    uint64_t retVal;
    retVal = currentVCB_p->nextIdToIssue;
    currentVCB_p->nextIdToIssue = currentVCB_p->nextIdToIssue + FILEIDINCREMENT;
    return (retVal);
  }

void flipFreeBlockBit (char * freeBuffer, uint64_t start, uint64_t count)
  {
    unsigned char * p;
    uint64_t byteStart;
    uint64_t bitnum;        //which bit within that byte
    unsigned char flipper;  //initial is 1000 0x00000001
    //The start is the "block number" which in our case with a 1:1 mapping is
    // the bit number
    //Divide by 8 to determine which byte number to start at.
    while (count > 0)
     {
       byteStart = start / 8;
       bitnum = start % 8;    //which bit within that byte
       flipper = 0x80;        //initial is 1000 0000

       if (bitnum > 0) {
          }
            flipper = flipper >> bitnum;  //shift bit within byte
            printf("Changing bit #%lu of the %lu byte in free block with flipper %X\n",
                      bitnum, byteStart, flipper);

            p = (unsigned char *)freeBuffer + byteStart;
            *p = *p ^ flipper;
            ++start;
            --count;
            
          }
  }

//helper method incase of overflow
uint64_t getFreeSpace (uint64_t blocksNeeded, int blockType)
  {
    uint64_t retVal = currentVCB_p->freeBlockLastAllocatedBit;
    if (blocksNeeded < currentVCB_p->freeBlockEndBlocksRemaining)
      {
        //easy allocation
        currentVCB_p->freeBlockLastAllocatedBit = currentVCB_p->freeBlockLastAllocatedBit + blocksNeeded;
        currentVCB_p->freeBlockEndBlocksRemaining = currentVCB_p->freeBlockEndBlocksRemaining - blocksNeeded;
        currentVCB_p->freeBlockTotalFreeBlocks = currentVCB_p->freeBlockTotalFreeBlocks - blocksNeeded;
        flipFreeBlockBit (currentVCB_p->freeBuffer, retVal, blocksNeeded);
        LBAwrite (currentVCB_p->freeBuffer, currentVCB_p->freeBlockBlocks, currentVCB_p->freeBlockLocation); //writes to disk
      }
      return (retVal);
  }

char * initFreemap (uint64_t volumeSize, uint64_t blockSize, uint64_t startPos)
  {
    uint64_t blocks = volumeSize / blockSize;
    //since there are 8 bits in a byte, we need blocks/8 bytes
    uint64_t freeBytesNeeded = (blocks / 8) + 1;

    //divide bytesNeeded by the block size to see how many blocks are needed for the freespace
    uint64_t freeBlocksNeeded = (freeBytesNeeded / blockSize) + 1;

    printf("Total Blocks: %lu; Bytes: %lu; FreeBlocksNeeded: %lu\n",
             blocks, freeBytesNeeded, freeBlocksNeeded);
    

    
    char * freeBuffer = malloc (freeBlocksNeeded * blockSize);
    memset (freeBuffer, 0xFF, freeBlocksNeeded * blockSize);
    flipFreeBlockBit (freeBuffer, 0, 1);  //always clear out the VCB blocks
    flipFreeBlockBit  (freeBuffer, startPos, freeBlocksNeeded); //clear Freemap blocks
    LBAwrite (freeBuffer, freeBlocksNeeded, startPos); //write to disk

    currentVCB_p->freeBlockBlocks = freeBlocksNeeded;
    currentVCB_p->freeBlockLastAllocatedBit = startPos + freeBlocksNeeded;
    currentVCB_p->freeBlockEndBlocksRemaining = blocks - currentVCB_p->freeBlockLastAllocatedBit;
    currentVCB_p->freeBlockTotalFreeBlocks = currentVCB_p->freeBlockEndBlocksRemaining;
    return  (freeBuffer);
  }

void freemap (uint64_t volumeSize, uint64_t blockSize)
  {
    currentVCB_p->freeBuffer = initFreemap (volumeSize, blockSize, 1);
    currentVCB_p->freeBlockLocation = 1;
  }


void initDir (uint64_t blockSize, dirEntry_p parent)
  {
    //if parent == NULL then this is the root
    dirEntry_p dirBuffer_p;

    uint64_t entrySize = sizeof(dirEntry);
    uint64_t bytesNeeded = AVGDIRECTORYENTRIES * entrySize;
    uint64_t blocksNeeded = (bytesNeeded + (blockSize - 1)) / blockSize;
    actualDirEntries = (blocksNeeded * blockSize) / entrySize;

    printf("For %d entries, we need %lu bytes, each entry is %lu  bytes\n",
              AVGDIRECTORYENTRIES, bytesNeeded, entrySize);
    printf("Actual directory entries = %lu\n", actualDirEntries);

    //i know how many blocks i need, ask the freeSystem for them
    uint64_t startLocation = getFreeSpace (blocksNeeded, CONTIG);

    dirBuffer_p = malloc(blocksNeeded * blockSize);

    //init all the directory entries
    printf("About to Clear all entries\n");
    for (int i = 0; i < actualDirEntries; i++) {
      dirBuffer_p[i].Id = 0;
      dirBuffer_p[i].flags = DIRFLAG_UNUSED;
      strcpy(dirBuffer_p[i].name, "");
      dirBuffer_p[i].date = 0;
      dirBuffer_p[i].location = 0;
      dirBuffer_p[i].sizeInBytes = 0;
    }
    time_t eTime;

    printf("About to setSelf\n");
    //set self entry and parent entry
    dirBuffer_p[0].Id = getNewFileID();
    printf("Just set Id\n");
    dirBuffer_p[0].flags = DIRFILE_DIRECTORY;
    printf("Just set the Flags\n");
    strcpy(dirBuffer_p[0].name, ".");
    printf("Just set the name\n");
    dirBuffer_p[0].date = time(&eTime);     //Now
    printf("Just set the time\n");
    dirBuffer_p[0].location = startLocation;
    printf("Just set the startLocation\n");
    dirBuffer_p[0].sizeInBytes = actualDirEntries * entrySize; //or this could be used
    printf("Just set the size\n");

      if(parent == NULL)  //if root, parent = self
        {
          parent = &(dirBuffer_p[0]);
        }
      printf("About to set parent\n");
      dirBuffer_p[1].Id = parent->Id;
      dirBuffer_p[1].flags = parent->flags;
      strcpy(dirBuffer_p[1].name, "..");
      dirBuffer_p[1].date = parent->date;
      dirBuffer_p[1].location = parent->location;
      dirBuffer_p[1].sizeInBytes = parent->sizeInBytes;

      printf("About to wite directory\n");
      LBAwrite (dirBuffer_p, blocksNeeded, startLocation);
  }

void initRootDir (/*uint64_t startLocation,*/ uint64_t blockSize)
  {
    dirEntry_p rootDirBuffer_p;

    uint64_t entrySize = sizeof(dirEntry);
    uint64_t bytesNeeded = AVGDIRECTORYENTRIES * entrySize;
    uint64_t blocksNeeded = (bytesNeeded * blockSize) / entrySize;

    printf("For %d entries, we need %lu bytes, each entry is %lu bytes\n",
              AVGDIRECTORYENTRIES, bytesNeeded, entrySize);
    printf("Actual directory entries = %lu\n", actualDirEntries);

    // I know how many blocks i need, ask the freeSystem for them
    uint64_t startLocation = getFreeSpace (blocksNeeded, CONTIG);

    rootDirBuffer_p = malloc(blocksNeeded * blockSize);

    //init all the directory entries
    for(int i = 0; i < actualDirEntries; i++)
      {
        rootDirBuffer_p[i].Id = 0;
        rootDirBuffer_p[i].flags = DIRFLAG_UNUSED;
        strcpy (rootDirBuffer_p[i].name, "");
        rootDirBuffer_p[i].date = 0;
        rootDirBuffer_p[i].location = 0;
        rootDirBuffer_p[i].sizeInBytes = 0;
      }

    rootDirBuffer_p[0].Id = 1000;
    rootDirBuffer_p[0].flags = 0;
    strcpy(rootDirBuffer_p[0].name, "..");
    rootDirBuffer_p[0].date = 45654;
    rootDirBuffer_p[0].location = startLocation;
    rootDirBuffer_p[0].sizeInBytes = actualDirEntries * entrySize;

    currentVCB_p->rootDirectoryStart = startLocation;

    LBAwrite (rootDirBuffer_p, blocksNeeded, startLocation);
  }



/*typedef struct badStruct
  {
    int counter;
    int size;
    char * name;
  } badStruct, * badStruct_p;

typedef struct goodStruct
  {
    int counter;
    int size;
    char * name;  // value on disk is ignored
  }goodStruct, * goodStruct_p;

*/
  typedef struct verygoodStruct
    {
      int counter;
      int size;
      char spacer[256-(sizeof(int)*2)];
      char name[]; //valuie on disk ignored
    }verygoodStruct, * verygoodStruct_p;

    int myfsOpen(char * filename, int method)
      {
        printf("%s\n", "We are in myfsOpen");
        int fd;
        int i;
        //get a file descriptor
        for(i = 0; i < FDOPENMAX; i++)
          {
            if (openFileList[i].flags == FDOPENFREE)
            {
              fd = i;
              break;
            }
          }

          //find file in directory
          //null, file does not exist
          //
          openFileList[i].flags = FDOPENINUSE | FDOPENFORREAD | FDOPENFORWRITE;
          openFileList[i].filebuffer = malloc (currentVCB_p-> blockSize * 2);
          openFileList[i].pointer = 0;  //seek is beginning of FILEIDINCREMENT
          openFileList[i].size = 0;

          return (fd);
      }

#define MYSEEK_CUR  1
#define MYSEEK_POS  2
#define MYSEEK_END  3

int myfsSeek(int fd, uint64_t position, int method)
  {
    //make sure fd is in use
    if(fd >= FDOPENMAX)
      return-1;

    if((openFileList[fd].flags & FDOPENINUSE) != FDOPENINUSE)
      return -1;

      switch (method) {
        case MYSEEK_CUR:
            openFileList[fd].position += position;
            break;

        case MYSEEK_POS:
            openFileList[fd].position = position;
            break;

        case MYSEEK_END:
            openFileList[fd].position = openFileList[fd].size + position;
            break;

        default:
            break;
      }
      return (openFileList[fd].position);
  }

  uint64_t myfsWrite (int fd, char * src, uint64_t length)
    {
      if(fd >= FDOPENMAX)
        return-1;

      if((openFileList[fd].flags & FDOPENINUSE) != FDOPENINUSE)
        return -1;

//this value below was named currentVCB_p and was throwing a lot of errors i believe due to naming overlaps, not sure what the value is used for yet
      uint64_t currentVCB = openFileList[fd].position / currentVCB_p->blockSize;
      uint64_t currentOffset = openFileList[fd].position % currentVCB_p->blockSize;

      if(length + currentOffset < currentVCB_p->blockSize)
        {
          //we are still in our buffer
          memcpy (openFileList[fd].filebuffer + currentOffset, src, length);
        }
      else if (length + currentOffset < currentVCB_p->blockSize * 2)
      {
        memcpy (openFileList[fd].filebuffer + currentOffset, src, length);
        //writeblock = translateFileBlock(fd, currentBlock);

        LBAwrite(openFileList[fd].filebuffer, 1, currentVCB + openFileList[fd].blockStart);
        memcpy (openFileList[fd].filebuffer, openFileList[fd].filebuffer + currentVCB_p->blockSize, currentVCB_p-> blockSize);
        ++currentVCB;
        //currentOffset =
      }
      else
      {
        //todo?
      }

      openFileList[fd].position = openFileList[fd].position + length;
      currentVCB = openFileList[fd].position / currentVCB_p->blockSize;
      currentOffset = openFileList[fd].position % currentVCB_p->blockSize;

    }

    int myfsClose(char * filename)
      {
        printf("%s\n", "We are in myfsClose but i dont believe it is setup to work other than free memory");
        int fd;
        int i;
        if(fd >= FDOPENMAX)
        return-1;

      if((openFileList[fd].flags & FDOPENINUSE) != FDOPENINUSE)
        return -1;


        

        for(i = 0; i < FDOPENMAX; i++)
          {
            if (openFileList[i].flags != FDOPENFREE)
            {
              fd = i;
              break;
            }
          }

          free(openFileList[i].filebuffer);
          openFileList[i].pointer = 0;  //seek is beginning of FILEIDINCREMENT
          openFileList[i].size = 0;
          openFileList[i].flags = FDOPENFREE;

      }

    //here we need  mfsRead

    void printUserOptions()
    {
      printf("%s\n", "--------USER COMMANDS---------");
      printf("%s\n", "Template for Commands: 'What you will do' [command]");
      printf("%s\n", "Exit Program [:q]");
      printf("%s\n", "Open File [:o]");
      printf("%s\n", "Edit File [:t]");
      printf("%s\n", "Exit File [:e]");
      printf("%s\n", "Seek File [:s]");

    }

  int main (int argc, char *argv[])
    {

    
      char * filename;
      char * userInput;
      char * closeValue = ":q";
      /*const char *validInputValues[5];
      validInputValues[0] = ":h";
      validInputValues[1] = ":r";
      validInputValues[2] = ":t";
      validInputValues[3] = ":p";
      validInputValues[4] = ":n";*/
      

      uint64_t volumeSize;
      uint64_t blockSize;
      int retVal;

      if(argc > 3)
        {
          filename = argv[1];
          volumeSize = atoll (argv[2]);
          blockSize = atoll (argv[3]);
        }
        else{
            printf("No command line inputs\n");
            printf("Default values will be used\n");
            filename = "Virtual Harddrive";
            volumeSize = 10000000;
            blockSize = 512;

        }
        uint64_t entrySize = sizeof(dirEntry);
        uint64_t bytesNeeded = AVGDIRECTORYENTRIES * entrySize;
        uint64_t blocksNeeded = (bytesNeeded * blockSize) / entrySize;

        retVal = startPartitionSystem (filename, &volumeSize, &blockSize);
        printf("Opened %s, Volume Size: %llu;  BlockSize: %llu; Return %d\n", filename, (ull_t)volumeSize, (ull_t)blockSize, retVal);
        
        initVolumeControlBlock(blockSize, volumeSize);
        printf("Set Volume Control Block\n");

        initRootDir(/*getFreeSpace(1, CONTIG),*/ blockSize);

        freemap(volumeSize, blockSize);

        openFileList = malloc(blocksNeeded * blockSize);
        
        //printf("%s\n" ,testString);
	
  //here we are getting an error when we reduce the volume size below 1000000 i think
	char * buf = malloc(blockSize *2);
  //printf("%s\n" ,testString);
	char * buf2 = malloc(blockSize *2);
  //printf("%s\n" ,testString);
	memset (buf, 0, blockSize*2);
	strcpy (buf, "Now is the time for all good people to come to the aid of their countrymen\n");
	strcpy (&buf[blockSize+10], "Four score and seven years ago our fathers brought forth onto this continent a new nation\n");
	LBAwrite (buf, 2, 0);
	LBAwrite (buf, 2, 3);
	LBAread (buf2, 2, 0);
	if (memcmp(buf, buf2, blockSize*2)==0)
		{
		printf("Read/Write worked\n");
		}
	else
		printf("FAILURE on Write/Read\n");


  printf("Enter in ':h' for help\n");

  while(strncmp(userInput, closeValue, strlen(closeValue)) != 0)
  {
    

    printf("Please enter in a command: ");
    fgets(userInput, 128, stdin);
    
    int length = strlen(userInput);
    /*printf("%s", userInput);
    printf("\n");
    printf("%d", length);
    printf("\n");*/

//user input logic
if(strncmp(userInput, ":h", strlen(":h")) == 0)
{
  printUserOptions();
}else if(strncmp(userInput, ":o", strlen(":o")) == 0)
{
  printf("Please enter in a filename to open: ");
  fgets(userInput, 512, stdin);
  
  uint64_t position;
  //printf("%s\n", "myfsOpen gives a segmentation fault i believe because we dont initialize our openFileList");
  
  myfsSeek(myfsOpen(userInput, CONTIG), 0, CONTIG);
  

}else if(strncmp(userInput, ":e", strlen(":e")) == 0)
{
  printf("Please enter in a filename to close: ");
  fgets(userInput, 512, stdin);
  //printf("%s\n", "myfsOpen gives a segmentation fault i believe because we dont initialize our openFileList");
  myfsClose(userInput);
}else if(strncmp(userInput, ":s", strlen(":s")) == 0)
{
  printf("Please enter in a filename to seek: ");
  fgets(userInput, 512, stdin);
  //printf("%s\n", "myfsOpen gives a segmentation fault i believe because we dont initialize our openFileList");
  //myfsSeek(userInput);
}else if(strncmp(userInput, ":q", strlen(":q")) == 0)
{
  printf("Closing Virtual Drive\n");
}else
{
  printf("Unrecognized Command\n");
}
  


}

		
	free (buf);
	free(buf2);
  free(currentVCB_p);
  free(openFileList);
	closePartitionSystem();
	return 0;
    }
