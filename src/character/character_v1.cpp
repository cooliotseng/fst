/*
  fst - An R-package for ultra fast storage and retrieval of datasets.
  Copyright (C) 2017, Mark AJ Klik

  BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  You can contact the author at :
  - fst source repository : https://github.com/fstPackage/fst
*/

#include <character_v1.h>

#include <Rcpp.h>
#include <iostream>
#include <fstream>

#include "lowerbound.h"
#include <compression.h>
#include <compressor.h>


// External libraries
#include "lz4.h"
// #include <boost/unordered_map.hpp>

using namespace std;
using namespace Rcpp;


#define BLOCKSIZE_CHAR 2047  // number of characters in default compression block
#define MAX_CHAR_STACK_SIZE 32768  // number of characters in default compression block
#define CHAR_HEADER_SIZE 8  // meta data header size
#define CHAR_INDEX_SIZE 16  // size of 1 index entry


inline void ReadDataBlockInfo(SEXP &strVec, unsigned long long blockSize, unsigned int nrOfElements,
  unsigned int startElem, unsigned int endElem, unsigned int vecOffset, unsigned int* sizeMeta, char* buf, unsigned int nrOfNAInts)
{
  unsigned int* strSizes = &sizeMeta[nrOfNAInts];
  unsigned int pos = 0;

  if (startElem != 0)
  {
    pos = strSizes[startElem - 1];  // offset previous element
  }

  // Test NA flag
  unsigned int flagNA = sizeMeta[nrOfNAInts - 1] & (1 << (nrOfElements % 32));
  if (flagNA == 0)  // no NA's in vector
  {
    for (unsigned int blockElem = startElem; blockElem <= endElem; ++blockElem)
    {
      unsigned int newPos = strSizes[blockElem];
      SEXP curStr = Rf_mkCharLen(buf + pos, newPos - pos);
      SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, curStr);
      pos = newPos;  // update to new string offset
    }

    return;
  }

  // We process the datablock in cycles of 32 strings. This minimizes the impact of NA testing for vectors with a small number of NA's

  unsigned int startCycle = startElem / 32;
  unsigned int endCycle = endElem / 32;
  unsigned int cycleNAs = sizeMeta[startCycle];

  // A single 32 string cycle

  if (startCycle == endCycle)
  {
    for (unsigned int blockElem = startElem; blockElem <= endElem; ++blockElem)
    {
      unsigned int bitMask = 1 << (blockElem % 32);

      if ((cycleNAs & bitMask) != 0)  // set string to NA
      {
        SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, NA_STRING);
        pos = strSizes[blockElem];  // update to new string offset
        continue;
      }

      // Get string from data stream

      unsigned int newPos = strSizes[blockElem];
      SEXP curStr = Rf_mkCharLen(buf + pos, newPos - pos);
      SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, curStr);
      pos = newPos;  // update to new string offset
    }

    return;
  }

  // Get possibly partial first cycle

  unsigned int firstCylceEnd = startCycle * 32 + 31;
  for (unsigned int blockElem = startElem; blockElem <= firstCylceEnd; ++blockElem)
  {
    unsigned int bitMask = 1 << (blockElem % 32);

    if ((cycleNAs & bitMask) != 0)  // set string to NA
    {
      SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, NA_STRING);
      pos = strSizes[blockElem];  // update to new string offset
      continue;
    }

    // Get string from data stream

    unsigned int newPos = strSizes[blockElem];
    SEXP curStr = Rf_mkCharLen(buf + pos, newPos - pos);
    SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, curStr);
    pos = newPos;  // update to new string offset
  }

  // Get all but last cycle with fast NA test

  for (unsigned int cycle = startCycle + 1; cycle != endCycle; ++cycle)
  {
    unsigned int cycleNAs = sizeMeta[cycle];
    unsigned int middleCycleEnd = cycle * 32 + 32;

    if (cycleNAs == 0)  // no NA's
    {
      for (unsigned int blockElem = cycle * 32; blockElem != middleCycleEnd; ++blockElem)
      {
        unsigned int newPos = strSizes[blockElem];
        SEXP curStr = Rf_mkCharLen(buf + pos, newPos - pos);
        SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, curStr);
        pos = newPos;  // update to new string offset
      }
      continue;
    }

    // Cycle contains one or more NA's

    for (unsigned int blockElem = cycle * 32; blockElem != middleCycleEnd; ++blockElem)
    {
      unsigned int bitMask = 1 << (blockElem % 32);
      unsigned int newPos = strSizes[blockElem];

      if ((cycleNAs & bitMask) != 0)  // set string to NA
      {
        SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, NA_STRING);
        pos = newPos;  // update to new string offset
        continue;
      }

      // Get string from data stream

      SEXP curStr = Rf_mkCharLen(buf + pos, newPos - pos);
      SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, curStr);
      pos = newPos;  // update to new string offset
    }
  }

  // Last cycle

  cycleNAs = sizeMeta[endCycle];

  ++endElem;
  for (unsigned int blockElem = endCycle * 32; blockElem != endElem; ++blockElem)
  {
    unsigned int bitMask = 1 << (blockElem % 32);
    unsigned int newPos = strSizes[blockElem];

    if ((cycleNAs & bitMask) != 0)  // set string to NA
    {
      SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, NA_STRING);
      pos = newPos;  // update to new string offset
      continue;
    }

    // Get string from data stream

    SEXP curStr = Rf_mkCharLen(buf + pos, newPos - pos);
    SET_STRING_ELT(strVec, vecOffset + blockElem - startElem, curStr);
    pos = newPos;  // update to new string offset
  }
}


inline void ReadDataBlock(ifstream &myfile, SEXP &strVec, unsigned long long blockSize, unsigned int nrOfElements,
  unsigned int startElem, unsigned int endElem, unsigned int vecOffset)
{
  unsigned int nrOfNAInts = 1 + nrOfElements / 32;  // last bit is NA flag
  unsigned int totElements = nrOfElements + nrOfNAInts;
  unsigned int *sizeMeta = new unsigned int[totElements];
  myfile.read((char*) sizeMeta, totElements * 4);  // read cumulative string lengths

  unsigned int charDataSize = blockSize - totElements * 4;

  char* buf = new char[charDataSize];
  myfile.read(buf, charDataSize);  // read string lengths

  ReadDataBlockInfo(strVec, blockSize, nrOfElements, startElem, endElem, vecOffset, sizeMeta, buf, nrOfNAInts);

  delete[] sizeMeta;
}


inline SEXP ReadDataBlockCompressed(ifstream &myfile, SEXP &strVec, unsigned long long blockSize, unsigned int nrOfElements,
  unsigned int startElem, unsigned int endElem, unsigned int vecOffset,
  unsigned int intBlockSize, Decompressor &decompressor, unsigned short int &algoInt, unsigned short int &algoChar)
{
  unsigned int nrOfNAInts = 1 + nrOfElements / 32;  // NA metadata including overall NA bit
  unsigned int totElements = nrOfElements + nrOfNAInts;
  unsigned int *sizeMeta = new unsigned int[totElements];

  // Read and uncompress str sizes data
  if (algoInt == 0)  // uncompressed
  {
    myfile.read((char*) sizeMeta, totElements * 4);  // read cumulative string lengths
  }
  else
  {
    myfile.read((char*) sizeMeta, nrOfNAInts * 4);  // read cumulative string lengths
    unsigned int intBufSize = intBlockSize;
    char *strSizeBuf = new char[intBufSize];
    myfile.read(strSizeBuf, intBufSize);

    // Decompress size but not NA metadata (which is currently uncompressed)

    decompressor.Decompress(algoInt, (char*)(&sizeMeta[nrOfNAInts]), nrOfElements * 4,
      strSizeBuf, intBlockSize);

    delete[] strSizeBuf;
  }

  unsigned int charDataSizeUncompressed = sizeMeta[nrOfNAInts + nrOfElements - 1];

  // Read and uncompress string vector data, use stack if possible here !!!!!
  unsigned int charDataSize = blockSize - intBlockSize - nrOfNAInts * 4;
  char* buf = new char[charDataSizeUncompressed];

  if (algoChar == 0)
  {
    myfile.read(buf, charDataSize);  // read string lengths
  }
  else
  {
    char* bufCompressed = new char[charDataSize];
    myfile.read(bufCompressed, charDataSize);  // read string lengths
    decompressor.Decompress(algoChar, buf, charDataSizeUncompressed, bufCompressed, charDataSize);
    delete[] bufCompressed;
  }

  ReadDataBlockInfo(strVec, blockSize, nrOfElements, startElem, endElem, vecOffset, sizeMeta, buf, nrOfNAInts);

  delete[] buf;  // character vector buffer
  delete[] sizeMeta;

  return List::create(
    _["startElem"] = startElem,
    _["endElem"] = endElem,
    _["algoInt"] = algoInt,
    _["charDataSize"] = charDataSize,
    _["charDataSizeUncompressed"] = charDataSizeUncompressed,
    _["algoChar"] = algoChar,
    _["intBlockSize"] = intBlockSize,
    _["nrOfElements"] = nrOfElements);
}


List fdsReadCharVec_v1(ifstream &myfile, SEXP &strVec, unsigned long long blockPos, unsigned int startRow, unsigned int vecLength, unsigned int size)
{
  // Jump to startRow size
  myfile.seekg(blockPos);

  // Read algorithm type and block size
  unsigned int meta[2];
  myfile.read((char*) meta, CHAR_HEADER_SIZE);

  unsigned int blockSizeChar = meta[1];
  unsigned int totNrOfBlocks = (size - 1) / blockSizeChar;  // total number of blocks minus 1
  unsigned int startBlock = startRow / blockSizeChar;
  unsigned int startOffset = startRow - (startBlock * blockSizeChar);
  unsigned int endBlock = (startRow + vecLength - 1)  / blockSizeChar;
  unsigned int endOffset = (startRow + vecLength - 1)  -  endBlock *blockSizeChar;
  unsigned int nrOfBlocks = 1 + endBlock - startBlock;  // total number of blocks to read

  // Vector data is uncompressed

  if (meta[0] == 0)
  {
    unsigned long long *blockOffset = new unsigned long long[1 + nrOfBlocks];  // block positions

    if (startBlock > 0)  // include previous block offset
    {
      myfile.seekg(blockPos + CHAR_HEADER_SIZE + (startBlock - 1) * 8);  // jump to correct block index
      myfile.read((char*) blockOffset, (1 + nrOfBlocks) * 8);
    }
    else
    {
      blockOffset[0] = CHAR_HEADER_SIZE + (totNrOfBlocks + 1) * 8;
      myfile.read((char*) &blockOffset[1], nrOfBlocks * 8);
    }


    // Navigate to first selected data block
    unsigned long long offset = blockOffset[0];
    myfile.seekg(blockPos + offset);

    unsigned int endElem = blockSizeChar - 1;
    unsigned int nrOfElements = blockSizeChar;

    if (startBlock == endBlock)  // subset start and end of block
    {
      endElem = endOffset;
      if (endBlock == totNrOfBlocks)
      {
        nrOfElements = size - totNrOfBlocks * blockSizeChar;  // last block can have less elements
      }
    }

    // Read first block with offset
    unsigned long long blockSize = blockOffset[1] - offset;  // size of data block

    ReadDataBlock(myfile, strVec, blockSize, nrOfElements, startOffset, endElem, 0);

    if (startBlock == endBlock)  // subset start and end of block
    {
      delete[] blockOffset;

      return List::create(
        _["res"] = "uncompressed",
        _["vecLength"] = vecLength,
        _["meta[0]"] = meta[0],
        _["meta[1]"] = meta[1],
        _["totNrOfBlocks"] = totNrOfBlocks,
        _["startBlock"] = startBlock,
        _["endBlock"] = endBlock,
        _["nrOfBlocks"] = nrOfBlocks,
        _["nrOfElements"] = (int) nrOfElements,
        _["endElem"] = (int) endElem,
        _["startOffset"] = startOffset,
        _["blockSize"] = blockSize,
        _["blockPos"] = (int) blockPos,
        _["blockOffset[0]"] = (int) blockOffset[0],
        _["blockOffset[1]"] = (int) blockOffset[1],
        _["blockOffset[2]"] = (int) blockOffset[2]);
    }

    offset = blockOffset[1];
    unsigned int vecPos = blockSizeChar - startOffset;

    if (endBlock == totNrOfBlocks)
    {
      nrOfElements = size - totNrOfBlocks * blockSizeChar;  // last block can have less elements
    }

    --nrOfBlocks;  // iterate full blocks
    for (unsigned int block = 1; block < nrOfBlocks; ++block)
    {
      unsigned long long newPos = blockOffset[block + 1];
      ReadDataBlock(myfile, strVec, newPos - offset, blockSizeChar, 0, blockSizeChar - 1, vecPos);
      vecPos += blockSizeChar;
      offset = newPos;
    }

    unsigned long long newPos = blockOffset[nrOfBlocks + 1];
    ReadDataBlock(myfile, strVec, newPos - offset, nrOfElements, 0, endOffset, vecPos);

    delete[] blockOffset;

    return List::create(
      _["res"] = "uncompressed",
      _["vecLength"] = vecLength,
      _["meta[0]"] = meta[0],
      _["meta[1]"] = meta[1],
      _["totNrOfBlocks"] = totNrOfBlocks,
      _["startBlock"] = startBlock,
      _["endBlock"] = endBlock,
      _["nrOfBlocks"] = nrOfBlocks,
      _["nrOfElements"] = (int) nrOfElements,
      _["endElem"] = (int) endElem,
      _["startOffset"] = startOffset,
      _["blockSize"] = blockSize,
      _["blockPos"] = (int) blockPos,
      _["blockOffset[0]"] = (int) blockOffset[0],
      _["blockOffset[1]"] = (int) blockOffset[1],
      _["blockOffset[2]"] = (int) blockOffset[2]);
  }


  // Vector data is compressed

  unsigned int bufLength = (nrOfBlocks + 1) * CHAR_INDEX_SIZE;  // 1 long and 2 unsigned int per block
  char *blockInfo = new char[bufLength + CHAR_INDEX_SIZE];  // add extra first element for convenience

  // unsigned long long blockOffset[1 + nrOfBlocks];  // block positions, algorithm and size information

  if (startBlock > 0)  // include previous block offset
  {
    myfile.seekg(blockPos + CHAR_HEADER_SIZE + (startBlock - 1) * CHAR_INDEX_SIZE);  // jump to correct block index
    myfile.read(blockInfo, (nrOfBlocks + 1) * CHAR_INDEX_SIZE);
  }
  else
  {
    unsigned long long* firstBlock = (unsigned long long*) blockInfo;
    *firstBlock = CHAR_HEADER_SIZE + (totNrOfBlocks + 1) * CHAR_INDEX_SIZE;  // offset of first data block
    myfile.read(&blockInfo[CHAR_INDEX_SIZE], nrOfBlocks * CHAR_INDEX_SIZE);
  }

  // Get block meta data
  unsigned long long* offset = (unsigned long long*) blockInfo;
  char* blockP = &blockInfo[CHAR_INDEX_SIZE];
  unsigned long long* curBlockPos = (unsigned long long*) blockP;
  unsigned short int* algoInt  = (unsigned short int*) (blockP + 8);
  unsigned short int* algoChar = (unsigned short int*) (blockP + 10);
  int* intBufSize = (int*) (blockP + 12);

  // move to first data block

  myfile.seekg(blockPos + *offset);

  unsigned int endElem = blockSizeChar - 1;
  unsigned int nrOfElements = blockSizeChar;

  if (startBlock == endBlock)  // subset start and end of block
  {
    endElem = endOffset;
    if (endBlock == totNrOfBlocks)
    {
      nrOfElements = size - totNrOfBlocks * blockSizeChar;  // last block can have less elements
    }
  }

  // Read first block with offset
  unsigned long long blockSize = *curBlockPos - *offset;  // size of data block

  Decompressor decompressor;  // uncompress all availble algorithms

  SEXP res = ReadDataBlockCompressed(myfile, strVec, blockSize, nrOfElements, startOffset, endElem, 0, *intBufSize,
    decompressor, *algoInt, *algoChar);

  if (startBlock == endBlock)  // subset start and end of block
  {
    delete[] blockInfo;

    return List::create(
      _["res"] = res,
      _["vecLength"] = vecLength,
      _["meta[0]"] = meta[0],
      _["meta[1]"] = meta[1],
      _["totNrOfBlocks"] = totNrOfBlocks,
      _["startBlock"] = startBlock,
      _["endBlock"] = endBlock,
      _["nrOfBlocks"] = nrOfBlocks,
      _["nrOfElements"] = (int) nrOfElements,
      _["endElem"] = (int) endElem,
      _["startOffset"] = startOffset,
      _["blockSize"] = blockSize,
      _["blockPos"] = (int) blockPos,
      _["*intBufSize"] = *intBufSize,
      _["*algoInt"] = *algoInt,
      _["*algoChar"] = *algoChar,
      _["*offset"] = *offset,
      _["*curBlockPos"] = *curBlockPos);
  }

  offset = curBlockPos;

  unsigned int vecPos = blockSizeChar - startOffset;

  if (endBlock == totNrOfBlocks)
  {
    nrOfElements = size - totNrOfBlocks * blockSizeChar;  // last block can have less elements
  }

  --nrOfBlocks;  // iterate all but last block
  blockP += CHAR_INDEX_SIZE;  // move to next index element
  for (unsigned int block = 1; block < nrOfBlocks; ++block)
  {
    unsigned long long* curBlockPos = (unsigned long long*) blockP;
    unsigned short int* algoInt  = (unsigned short int*) (blockP + 8);
    unsigned short int* algoChar = (unsigned short int*) (blockP + 10);
    int* intBufSize = (int*) (blockP + 12);

    ReadDataBlockCompressed(myfile, strVec, *curBlockPos - *offset, blockSizeChar, 0, blockSizeChar - 1, vecPos, *intBufSize,
      decompressor, *algoInt, *algoChar);
    vecPos += blockSizeChar;
    offset = curBlockPos;
    blockP += CHAR_INDEX_SIZE;  // move to next index element
  }

  curBlockPos = (unsigned long long*) blockP;
  algoInt  = (unsigned short int*) (blockP + 8);
  algoChar = (unsigned short int*) (blockP + 10);
  intBufSize = (int*) (blockP + 12);

  ReadDataBlockCompressed(myfile, strVec, *curBlockPos - *offset, nrOfElements, 0, endOffset, vecPos, *intBufSize,
      decompressor, *algoInt, *algoChar);

  delete[] blockInfo;

  return List::create(
    _["vecLength"] = vecLength,
    _["meta[0]"] = meta[0],
    _["meta[1]"] = meta[1],
    _["meta[2]"] = meta[2]);
}
