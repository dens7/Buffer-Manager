/**
 * @author See Contributors.txt for code contributors and overview of BadgerDB.
 *
 * @section LICENSE
 * Copyright (c) 2012 Database Group, Computer Sciences Department, University of Wisconsin-Madison.
 */

#include <memory>
#include <iostream>
#include "buffer.h"
#include "exceptions/buffer_exceeded_exception.h"
#include "exceptions/page_not_pinned_exception.h"
#include "exceptions/page_pinned_exception.h"
#include "exceptions/bad_buffer_exception.h"
#include "exceptions/hash_not_found_exception.h"


namespace badgerdb { 

//----------------------------------------
// Constructor of the class BufMgr
//----------------------------------------

BufMgr::BufMgr(std::uint32_t bufs): numBufs(bufs) 
{
	bufDescTable = new BufDesc[bufs];
	for (FrameId i = 0; i < bufs; i++) 
  	{
  		bufDescTable[i].frameNo = i;
  		bufDescTable[i].valid = false;
  	}

  	bufPool = new Page[bufs];
	int htsize = ((((int) (bufs * 1.2))*2)/2)+1;
  	hashTable = new BufHashTbl (htsize);  // allocate the buffer hash table
  	clockHand = bufs - 1;
}


BufMgr::~BufMgr() 
{
	for(std::uint32_t i = 0; i < numBufs; i++)	//Flushes out all dirty pages
		if(bufDescTable[i].dirty) flushFile(bufDescTable[i].file);
		
	delete [] bufPool;
	delete [] bufDescTable;	
}

/*
 * Advance clock to next frame in the buffer pool.
 */
void BufMgr::advanceClock()
{
	clockHand++;
	clockHand %= numBufs;
}

/*
* Allocates a free frame using the clock algorithm; if necessary, writing a dirty page back
* to disk.  Throws BufferExceededException if all buffer frames are pinned.  This private
* method will get called by the readPage() and allocPage() methods described below.
*/
void BufMgr::allocBuf(FrameId & frame) 
{
	bool found = false;
	std::uint32_t i;
	for(i = 0; i < numBufs; i++) 
	{
		advanceClock();
		if (!bufDescTable[clockHand].valid) 
		{
			found = true;
			break;
		}
		else if (bufDescTable[clockHand].refbit) bufDescTable[clockHand].refbit = false;
		else if (bufDescTable[clockHand].pinCnt != 0);
		else 
		{
			found = true;
			hashTable->remove(bufDescTable[clockHand].file, bufDescTable[clockHand].pageNo);
			if (bufDescTable[clockHand].dirty)
			{
				bufDescTable[clockHand].dirty = false;
				bufDescTable[clockHand].file->writePage(bufPool[clockHand]); 
			}
		}
	}
	
	if (!found && i >= numBufs) throw BufferExceededException();
	bufDescTable[clockHand].Clear();
	frame = clockHand;	
}

	
/*Checks whether the page is already in the buffer pool by invoking the lookup() method,
which may throw HashNotFoundException when page is not in the buffer pool, on the
hashtable to get a frame number. Calls allocBuf() to allocate a buffer frame and
then call the method file->readPage() to read the page from disk into the buffer pool
frame.  Next, insert the page into the hashtable. Finally, invoke Set() on the frame to
set it up properly. Set() will leave the pinCnt for the page set to 1. Return a pointer
to the frame containing the page via the page parameter. If page is in the buffer pool, set the 
appropriate refbit, increment the pinCnt for the page, and then return a pointer to the frame containing the page
via the page parameter.*/
void BufMgr::readPage(File* file, const PageId pageNo, Page*& page)
{
	FrameId frameNum;
	try
	{
		hashTable->lookup(file, pageNo, frameNum);
		bufDescTable[frameNum].refbit = true;
		bufDescTable[frameNum].pinCnt += 1;
	}
	catch(HashNotFoundException e)
	{
		allocBuf(frameNum); 
		bufPool[frameNum] = file->readPage(pageNo);
		hashTable->insert(file, pageNo, frameNum);
		bufDescTable[frameNum].Set(file, pageNo);
	}
	
	page = &bufPool[frameNum];

}


/*
*Decrements the pinCnt of the frame containing (file, PageNo) and, if dirty == true, sets
*the dirty bit. Throws PAGENOTPINNED if the pin count is already 0. Does nothing if
*page is not found in the hash table lookup.
*/
void BufMgr::unPinPage(File* file, const PageId pageNo, const bool dirty) 
{
	FrameId frameNum;
	try 
	{
		hashTable->lookup(file, pageNo, frameNum);
		if (bufDescTable[frameNum].pinCnt == 0) 
			throw PageNotPinnedException(file->filename(), pageNo, frameNum);
		bufDescTable[frameNum].pinCnt--;
		if(dirty) bufDescTable[frameNum].dirty = true;
	}
	catch(HashNotFoundException e)
	{

	}
}

void BufMgr::allocPage(File* file, PageId &pageNo, Page*& page) 
{
	FrameId frameNum;
	allocBuf(frameNum);
	bufPool[frameNum] = file->allocatePage();
	pageNo = bufPool[frameNum].page_number();
	hashTable->insert(file, pageNo, frameNum);
	bufDescTable[frameNum].Set(file, pageNo);
	page = &bufPool[frameNum];
}

/*
 * Should scan bufTable for pages belonging to the file. For each page encountered it should:
 * (a) if the page is dirty, call file->writePage() to flush the page to disk and then set the dirty
 * bit for the page to false, (b) remove the page from the hashtable (whether the page is clean
 * or dirty) and (c) invoke the Clear() method of BufDesc for the page frame.
 * Throws PagePinnedException if some page of the file is pinned. Throws BadBufferException if an invalid page belonging to the file is encountered.
 * */
void BufMgr::flushFile(const File* file) 
{
	for(std::uint32_t i = 0; i < numBufs; i++)
	{
		if(bufDescTable[i].file == file && !bufDescTable[i].valid)
			throw BadBufferException(bufDescTable[i].frameNo, bufDescTable[i].dirty, bufDescTable[i].valid, bufDescTable[i].refbit);
		else if(bufDescTable[i].file == file && bufDescTable[i].valid)
		{
			if(bufDescTable[i].pinCnt != 0)
			{
				throw PagePinnedException(file->filename(), bufDescTable[i].pageNo, bufDescTable[i].frameNo);
			}
			if(bufDescTable[i].dirty)
			{
				bufDescTable[i].file->writePage(bufPool[bufDescTable[i].frameNo]);
				bufDescTable[i].dirty = false;
			}
			hashTable->remove(file, bufDescTable[i].pageNo);
			bufDescTable[i].Clear();
		}
		
	}
}

/*
This method deletes a particular page from file. Before deleting the page from file, it
makes sure that if the page to be deleted is allocated a frame in the buffer pool, that frame
is freed and correspondingly entry from hash table is also removed
*/
void BufMgr::disposePage(File* file, const PageId PageNo)
{
	//Check for page in the buffer pool
	FrameId frameNum;
	try 
	{
		hashTable->lookup(file, PageNo, frameNum);
		//if page is found delete it & write if necessary
		if (bufDescTable[frameNum].dirty) {
			bufDescTable[frameNum].dirty = false;
			file->writePage(bufPool[frameNum]);
		}
		//clear the frame
		bufDescTable[frameNum].Clear();
		hashTable->remove(file, PageNo);
		file->deletePage(PageNo);
	} catch (HashNotFoundException e) {
		file->deletePage(PageNo);
	}
}

void BufMgr::printSelf(void) 
{
  	BufDesc* tmpbuf;
	int validFrames = 0;
  
 	for (std::uint32_t i = 0; i < numBufs; i++) {
  		tmpbuf = &(bufDescTable[i]);
		std::cout << "FrameNo:" << i << " ";
		tmpbuf->Print();

  	if (tmpbuf->valid == true)
    	validFrames++;
}

	std::cout << "Total Number of Valid Frames:" << validFrames << "\n";
}

}
