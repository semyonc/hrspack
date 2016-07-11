//
//  hrspack.h
//  hrspack
//
//  Created by Semyon A. Chertkov on 04.07.14.
//  Copyright (c) 2014 Semyon A. Chertkov. All rights reserved.
//

#ifndef __hrspack__hrspack__
#define __hrspack__hrspack__

#if defined(__unix__) || defined(__unix) || defined(unix) || (defined(__APPLE__) && defined(__MACH__))
#define UNIX
#endif

#include <assert.h>
#include <time.h>
#include <memory.h>

#ifdef UNIX
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#endif

#if defined(__SSE2__) || (defined(_MSC_VER) && !defined(_M_CEE_PURE))
#define SIMD
#if defined(ENABLE_SSE4) || defined(_WIN32)
#include <emmintrin.h>
#include <tmmintrin.h>
#define mullo_epi32(a, b) _mm_mullo_epi32(a, b)
#else 
#include <emmintrin.h>
#define mullo_epi32(a, b) \
    _mm_unpacklo_epi32(_mm_shuffle_epi32(_mm_mul_epu32(a, b), 0xd8), \
    _mm_shuffle_epi32(_mm_mul_epu32(_mm_shuffle_epi32(a, 0xb1), \
    _mm_shuffle_epi32(b, 0xb1)), 0xd8))
#endif
#endif

#define DEFAULT_OPTION 3 // 9 4 3

// 8, 16, 32 bit unsigned types (adjust as appropriate)

#if defined( _WIN32 )
#if defined( _WIN64 )
#define SYSTEM_STR "Win64"
#else
#define SYSTEM_STR "Win32"
#endif
#elif defined( __linux__ )
#define SYSTEM_STR "Linux"
#elif defined( __APPLE__ )
#define SYSTEM_STR "Mac OS X"
#elif defined( __FreeBSD__ )
#define SYSTEM_STR "FreeBSD"
#else
#define SYSTEM_STR "(unknown)"
#endif


typedef long long LONG;
typedef float FLOAT;
typedef unsigned char Extended[10];

#ifdef __GNUC__

typedef __int32_t ID;
typedef __uint8_t  U8;
typedef __uint16_t U16;
typedef __uint32_t U32;

#define BINARY_SWAP16 __builtin_bswap16
#define BINARY_SWAP32 __builtin_bswap32
#define BINARY_SWAP64 __builtin_bswap64

#else

typedef __int32 ID;
typedef unsigned __int8  U8;
typedef unsigned __int16 U16;
typedef unsigned __int32 U32;

#define BINARY_SWAP16 _byteswap_ushort
#define BINARY_SWAP32 _byteswap_ulong
#define BINARY_SWAP64 _byteswap_uint64

#define aligned_alloc(align,size) _aligned_malloc(size,align)
#define aligned_free(ptr) _aligned_free(ptr)

#endif

#if WINDOWS
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define SWAP(a,b) {auto x=a;a=b;b=x;}
#endif

typedef struct
{
	std::streampos beg;			/* internal bookmark for rewrite */
	ID chkID;					/* chunk ID */
	int ckSize;					/* chunk Size */
} Chunk;

typedef struct  // Form
{
	ID chkID;			/* chunk ID */
	int ckSize;		/* chunk Size */
	ID formType;        /* Form ID */
} FormChunk;

typedef struct // Common
{
	Chunk h;
	short numChannels;
	unsigned numSampleFrames;
	short sampleSize;
	long double sampleRate;
} CommonChunk;

typedef struct { // SSND Chunk
	Chunk h;
	U32 offset;
	U32 blockSize;
} SoundDataChunk;


// Packed file header block

enum FileFlags {
	MEM_MASK = 0x000F,
	CRC = 0x0010,
	AIFF = 0x0100,
	WAV = 0x0200,
	RAW = 0x0300
};

typedef struct {
	ID id;
	U8 version;
	unsigned cbSize;
	U16 flags;
	U32 crc32;	
	unsigned frameSize;
} FileHeader;

#ifdef UNIX
#define APP_NAME "hrspack"
#else
#define APP_NAME "hrspack.exe"
#endif

#define FILE_EXT ".hrs"
#define APP_VERSION 1

#define RETURN_CODE_OK				0
#define RETURN_CODE_BAD_ARGUMENT	1
#define RETURN_CODE_BAD_FILE        2
#define RETURN_CODE_IO_ERROR		3
#define RETURN_CODE_CRC_MISMATCH	4
#define RETURN_CODE_VALIDATION		5


#define LOG
#define CHAR_BUFFER_SIZE 4096
#define NN_WINDOW_SIZE 512

extern std::ostream *verbose_output;
#define COUT (*verbose_output)

std::string getfilename(const std::string& str);

bool CheckOption(int argc, const char **argv, const char *opt);
int GetOptionValue(int argc, const char **argv, const char *opt, int default_value = 0);
int GetOptionValues(int argc, const char **argv, const char *opt, int N, int *val);
void showUsage();
void showHelp();

bool compareFile(const std::string &file1, const std::string &file2);

int decompress(const std::string &inputFile, const std::string &outputFile);

int compressAIFF(const std::string &inputFile, const std::string &outputFile);
int decompressAIFF(std::ifstream& ifs, const std::string &inputFile, std::string outputFile, FileHeader &hdr);

#endif /* defined(__hrspack__hrspack__) */
