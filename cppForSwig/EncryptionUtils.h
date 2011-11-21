////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011, Alan C. Reiner    <alan.reiner@gmail.com>             //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE or http://www.gnu.org/licenses/agpl.html                      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//
// Implements canned routines in Crypto++ for AES encryption (for wallet
// security), ECDSA (which is already available in the python interface,
// but it is slow, so we might as well use the fast C++ method if avail),
// time- and memory-hard key derivation functions (resistent to brute
// force, and designed to be too difficult for a GPU to implement), and
// secure binary data handling (to make sure we don't leave sensitive 
// data floating around in application memory).
//
// 
// For the KDF:
//
// This technique is described in Colin Percival's paper on memory-hard 
// key-derivation functions, used to create "scrypt":
//
//       http://www.tarsnap.com/scrypt/scrypt.pdf
//
// The goal is to create a key-derivation function that can force a memory
// requirement on the thread applying the KDF.  By picking a sequence-length
// of 1,000,000, each thread will require 32 MB of memory to compute the keys,
// which completely disarms GPUs of their massive parallelization capabilities
// (for maximum parallelization, the kernel must use less than 1-2 MB/thread)
//
// Even with less than 1,000,000 hashes, as long as it requires more than 64
// kB of memory, a GPU will have to store the computed lookup tables in global
// memory, which is extremely slow for random lookup.  As a result, GPUs are 
// no better (and possibly much worse) than a CPU for brute-forcing the passwd
//
// This KDF is actually the ROMIX algorithm described on page 6 of Colin's
// paper.  This was chosen because it is the simplest technique that provably
// achieves the goal of being secure, and memory-hard.
//
// The computeKdfParams method well test the speed of the system it is running
// on, and try to pick the largest memory-size the system can compute in less
// than 0.25s (or specified target).  
//
////////////////////////////////////////////////////////////////////////////////

#ifndef _ENCRYPTION_UTILS_
#define _ENCRYPTION_UTILS_

#include <iostream>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>

#include "cryptlib.h"
#include "osrng.h"
#include "sha.h"
#include "aes.h"
#include "modes.h"
#include "eccrypto.h"
#include "filters.h"

#include "BinaryData.h"
#include "BtcUtils.h"
#include "UniversalTimer.h"

// This is used to attempt to keep keying material out of swap
// I am stealing this from bitcoin 0.4.0 src, serialize.h
#ifdef _MSC_VER
   // Note that VirtualLock does not provide this as a guarantee on Windows,
   // but, in practice, memory that has been VirtualLock'd almost never gets written to
   // the pagefile except in rare circumstances where memory is extremely low.
   #include <windows.h>
   #define mlock(p, n) VirtualLock((p), (n));
   #define munlock(p, n) VirtualUnlock((p), (n));
#else
   #include <sys/mman.h>
   #include <limits.h>
   /* This comes from limits.h if it's not defined there set a sane default */
   #ifndef PAGESIZE
      #include <unistd.h>
      #define PAGESIZE sysconf(_SC_PAGESIZE)
   #endif
   #define mlock(a,b) \
     mlock(((void *)(((size_t)(a)) & (~((PAGESIZE)-1)))),\
     (((((size_t)(a)) + (b) - 1) | ((PAGESIZE) - 1)) + 1) - (((size_t)(a)) & (~((PAGESIZE) - 1))))
   #define munlock(a,b) \
     munlock(((void *)(((size_t)(a)) & (~((PAGESIZE)-1)))),\
     (((((size_t)(a)) + (b) - 1) | ((PAGESIZE) - 1)) + 1) - (((size_t)(a)) & (~((PAGESIZE) - 1))))
#endif


// We will look for a high memory value to use in the KDF
// But as a safety check, we should probably put a cap
// on how much memory the KDF can use -- 32 MB is good
// If a KDF uses 32 MB of memory, it is undeniably easier
// to computer on a CPU than a GPU.
#define DEFAULT_KDF_MAX_MEMORY 32*1024*1024

using namespace std;


// Use this to avoid "using namespace CryptoPP" (which confuses SWIG)
// and also so it's easy to switch the AES MODE or PRNG, in one place
#define BTC_AES      CryptoPP::AES
#define BTC_AES_MODE CryptoPP::CFB_Mode
#define BTC_PRNG     CryptoPP::AutoSeededRandomPool

#define BTC_ECPOINT  CryptoPP::ECP::Point
#define BTC_ECDSA    CryptoPP::ECDSA< CryptoPP::ECP, CryptoPP::SHA256 >
#define BTC_PRIVKEY  CryptoPP::ECDSA< CryptoPP::ECP, CryptoPP::SHA256 >::PrivateKey
#define BTC_PUBKEY   CryptoPP::ECDSA< CryptoPP::ECP, CryptoPP::SHA256 >::PublicKey
#define BTC_SIGNER   CryptoPP::ECDSA< CryptoPP::ECP, CryptoPP::SHA256 >::Signer 
#define BTC_VERIFIER CryptoPP::ECDSA< CryptoPP::ECP, CryptoPP::SHA256 >::Verifier


////////////////////////////////////////////////////////////////////////////////
// Make sure that all crypto information is handled with page-locked data,
// and overwritten when it's destructor is called.  For simplicity, we will
// use this data type for all crypto data, even for data values that aren't
// really sensitive.  We can use the SecureBinaryData(bdObj) to convert our 
// regular strings/BinaryData objects to secure objects
//
class SecureBinaryData : public BinaryData
{
public:
   // We want regular BinaryData, but page-locked and secure destruction
   SecureBinaryData(void) : BinaryData() 
                   { lockData(); }
   SecureBinaryData(uint32_t sz) : BinaryData(sz) 
                   { lockData(); }
   SecureBinaryData(BinaryData const & data) : BinaryData(data) 
                   { lockData(); }
   SecureBinaryData(uint8_t const * inData, size_t sz) : BinaryData(inData, sz)
                   { lockData(); }
   SecureBinaryData(uint8_t const * d0, uint8_t const * d1) : BinaryData(d0, d1)
                   { lockData(); }
   SecureBinaryData(string const & str) : BinaryData(str)
                   { lockData(); }
   SecureBinaryData(BinaryDataRef const & bdRef) : BinaryData(bdRef)
                   { lockData(); }

   ~SecureBinaryData(void) { destroy(); }


   SecureBinaryData(SecureBinaryData const & sbd2) : 
           BinaryData(sbd2.getPtr(), sbd2.getSize()) { lockData(); }


   void resize(size_t sz)  { BinaryData::resize(sz);  lockData(); }
   void reserve(size_t sz) { BinaryData::reserve(sz); lockData(); }


   BinaryData    getRawCopy(void) { return BinaryData(getPtr(),    getSize()); }
   BinaryDataRef getRawRef(void)  { return BinaryDataRef(getPtr(), getSize()); }

   SecureBinaryData & append(SecureBinaryData & sbd2) ;
   SecureBinaryData & operator=(SecureBinaryData const & sbd2);
   SecureBinaryData   operator+(SecureBinaryData & sbd2) const;
   //uint8_t const & operator[](size_t i) const {return BinaryData::operator[](i);}
   bool operator==(SecureBinaryData const & sbd2) const;

   static SecureBinaryData GenerateRandom(uint32_t numBytes);

   void lockData(void)
   {
      if(getSize() > 0)
         mlock(getPtr(), getSize());
   }

   void destroy(void)
   {
      if(getSize() > 0)
      {
         fill(0x00);
         munlock(getPtr(), getSize());
      }
   }

};




////////////////////////////////////////////////////////////////////////////////
// A memory-bound key-derivation function -- uses a variation of Colin 
// Percival's ROMix algorithm: http://www.tarsnap.com/scrypt/scrypt.pdf
//
// The computeKdfParams method takes in a target time, T, for computation
// on the computer executing the test.  The final KDF should take somewhere
// between T/2 and T seconds.
class KdfRomix
{
public:

   /////////////////////////////////////////////////////////////////////////////
   KdfRomix(void);

   /////////////////////////////////////////////////////////////////////////////
   KdfRomix(uint32_t memReqts, uint32_t numIter, SecureBinaryData salt);


   /////////////////////////////////////////////////////////////////////////////
   // Default max-memory reqt will 
   void computeKdfParams(double   targetComputeSec=0.25, 
                         uint32_t maxMemReqts=DEFAULT_KDF_MAX_MEMORY);

   /////////////////////////////////////////////////////////////////////////////
   void usePrecomputedKdfParams(uint32_t memReqts, 
                                uint32_t numIter, 
                                SecureBinaryData salt);

   /////////////////////////////////////////////////////////////////////////////
   void printKdfParams(void);

   /////////////////////////////////////////////////////////////////////////////
   SecureBinaryData DeriveKey_OneIter(SecureBinaryData const & password);

   /////////////////////////////////////////////////////////////////////////////
   SecureBinaryData DeriveKey(SecureBinaryData const & password);

   /////////////////////////////////////////////////////////////////////////////
   string       getHashFunctionName(void) const { return hashFunctionName_; }
   uint32_t     getMemoryReqtBytes(void) const  { return memoryReqtBytes_; }
   uint32_t     getNumIterations(void) const    { return numIterations_; }
   SecureBinaryData   getSalt(void) const       { return salt_; }
   
private:

   string   hashFunctionName_;  // name of hash function to use (only one)
   uint32_t hashOutputBytes_;
   uint32_t kdfOutputBytes_;    // size of final key data

   uint32_t memoryReqtBytes_;
   uint32_t sequenceCount_;
   SecureBinaryData lookupTable_;
   SecureBinaryData salt_;            // prob not necessary amidst numIter, memReqts
                                // but I guess it can't hurt

   uint32_t numIterations_;     // We set the ROMIX params for a given memory 
                                // req't. Then run it numIter times to meet
                                // the computation-time req't
};


////////////////////////////////////////////////////////////////////////////////
// Leverage CryptoPP library for AES encryption/decryption
class CryptoAES
{
public:
   CryptoAES(void) {}

   /////////////////////////////////////////////////////////////////////////////
   SecureBinaryData Encrypt(SecureBinaryData & data, 
                            SecureBinaryData & key,
                            SecureBinaryData & iv);

   /////////////////////////////////////////////////////////////////////////////
   SecureBinaryData Decrypt(SecureBinaryData & data, 
                            SecureBinaryData & key,
                            SecureBinaryData   iv);
};





////////////////////////////////////////////////////////////////////////////////
// Create a C++ interface to the Crypto++ ECDSA ops:  should be more secure
// and much faster than the pure-python methods created by Lis
//
// These methods might as well just be static methods, but SWIG doesn't like
// static methods.  So we will invoke these via CryptoECDSA().Function()
class CryptoECDSA
{
public:
   CryptoECDSA(void) {}

   /////////////////////////////////////////////////////////////////////////////
   BTC_PRIVKEY CreateNewPrivateKey(void);

   /////////////////////////////////////////////////////////////////////////////
   BTC_PRIVKEY ParsePrivateKey(SecureBinaryData const & privKeyData);
   
   /////////////////////////////////////////////////////////////////////////////
   BTC_PUBKEY ParsePublicKey(SecureBinaryData const & pubKey65B);

   /////////////////////////////////////////////////////////////////////////////
   BTC_PUBKEY ParsePublicKey(SecureBinaryData const & pubKeyX32B,
                             SecureBinaryData const & pubKeyY32B);
   
   /////////////////////////////////////////////////////////////////////////////
   SecureBinaryData SerializePrivateKey(BTC_PRIVKEY const & privKey);
   
   /////////////////////////////////////////////////////////////////////////////
   SecureBinaryData SerializePublicKey(BTC_PUBKEY const & pubKey);

   /////////////////////////////////////////////////////////////////////////////
   BTC_PUBKEY ComputePublicKey(BTC_PRIVKEY const & cppPrivKey);

   
   /////////////////////////////////////////////////////////////////////////////
   bool CheckPubPrivKeyMatch(BTC_PRIVKEY const & cppPrivKey,
                             BTC_PUBKEY  const & cppPubKey);
   
   /////////////////////////////////////////////////////////////////////////////
   // For signing and verification, pass in original, UN-HASHED binary string
   SecureBinaryData SignData(SecureBinaryData const & binToSign, 
                             BTC_PRIVKEY const & cppPrivKey);
   
   
   
   /////////////////////////////////////////////////////////////////////////////
   // For signing and verification, pass in original, UN-HASHED binary string
   bool VerifyData(SecureBinaryData const & binMessage, 
                   SecureBinaryData const & binSignature,
                   BTC_PUBKEY const & cppPubKey);


   /////////////////////////////////////////////////////////////////////////////
   // We need to make sure that we have methods that take only secure strings
   // and return secure strings (I don't feel like figuring out how to get 
   // SWIG to take BTC_PUBKEY and BTC_PRIVKEY

   SecureBinaryData GenerateNewPrivateKey(void);
   
   /////////////////////////////////////////////////////////////////////////////
   SecureBinaryData ComputePublicKey(SecureBinaryData const & cppPrivKey);

   /////////////////////////////////////////////////////////////////////////////
   bool CheckPubPrivKeyMatch(SecureBinaryData const & privKey32,
                             SecureBinaryData const & pubKey65);

   /////////////////////////////////////////////////////////////////////////////
   // For signing and verification, pass in original, UN-HASHED binary string
   SecureBinaryData SignData(SecureBinaryData const & binToSign, 
                             SecureBinaryData const & binPrivKey);
   
   /////////////////////////////////////////////////////////////////////////////
   // For signing and verification, pass in original, UN-HASHED binary string
   bool VerifyData(SecureBinaryData const & binMessage, 
                   SecureBinaryData const & binSignature,
                   SecureBinaryData const & pubkey65B);
                               
};


#endif













