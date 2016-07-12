//
//  hrspack is an experimental lossless codec and archiver 
//
//  Copyright (c) 2014-2016 Semyon A. Chertkov. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
// http://www.gnu.org/copyleft/gpl.html

#include "stdafx.h"
#include "hrspack.h"

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wshift-op-parentheses"
#pragma GCC diagnostic ignored "-Wdeprecated-register"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wbitwise-op-parentheses"
#pragma GCC diagnostic ignored "-Wignored-attributes"
#endif

#ifdef _MSC_VER
#pragma warning( disable : 4554 )
#endif

#define CRC_MASK           0xFFFFFFFFL
#define CRC32_POLYNOMIAL   0xEDB88320L

#define DEC(x) ((x & 1)?((x + 1) >> 1):(-x >> 1))
#define ENC(x) ((x > 0)?((x << 1) - 1):(-x << 1))
#define QUANTIZE(x, k)  (((x) + (1 << (k - 1))) >> k)
#define QUANTIZE64(x, k)  (((x) + (1LL << (k - 1))) >> k)

#if defined(DUMPLINES) && DUMPLINES > 0
static std::ofstream *dump;
static int lineNo;
#define BEGIN_DUMP(x) { dump = new std::ofstream(x); lineNo = 1; }
#define END_DUMP { dump->close(); delete dump; }
#define OUTVAL(x) if (lineNo <= DUMPLINES) \
	{ (*dump) << (x) << "\t"; }
#define OUTENDL if (lineNo <= DUMPLINES) \
	{ (*dump) << std::endl; lineNo++; }
#else
#define BEGIN_DUMP(x)
#define END_DUMP
#define OUTVAL(x) 
#define OUTENDL
#endif


extern bool calculateCRC;

// 32-bit pseudo random number generator
class Random {
	int i;
	U32 table[64];

public:
	Random()  {
		reset();
	}
	
	U32 next() {
		return ++i, table[i & 63] = table[i - 24 & 63] ^ table[i - 55 & 63];
	}

	FLOAT operator()() {
		int rand = (int)next();
		if (rand<0) rand += 2147483647;
		return rand / 2147483647.0f;
	}

	void reset() {
		static U32 table[64] = {
			123456789,	987654321,	2451732073,	1276824833,	1194875201,	483548284,	1131084193,	4010221277,
			1178081878,	201547279,	2299900032,	3845352629,	3729054079,	2523874489,	2253057177,	3447201116,	3577018127,	816007129,
			427833720,	510472047,	1398365281,	2694048858,	3995835561,	1119117060,	3827222110,	3711291039,	2302418428,	4086473432,
			2090035810,	1752718412,	2151669808,	2297145021,	3933994084,	405182459,	180011672,	2025707264,	1066710202,	3371477262,
			2917730149,	2044920923,	1186998922,	427305520,	501091638,	1294421495,	1537182874,	4274227889,	4129050552,	2708505670,
			4053688730,	1776283586,	2549283235,	2409800359,	912933895,	1695200797,	1705940233,	1606405420,	526960416,	1663321423,
			1337301155,	2068951714,	1326954592,	1732926696,	1910773506,	3913802403 };
		for (int k = 0; k < 64; k++)
			this->table[k] = table[k];
		i = 0;
	}

} xrnd;

class CRC32 {
private:	
	U32 table[256];

public:
	CRC32() {
		short i, j;
		unsigned int value;

		for (i = 0; i <= 255; i++)
		{
			value = i;
			for (j = 8; j > 0; j--)
			{
				if (value & 1)
					value = (value >> 1) ^ CRC32_POLYNOMIAL;
				else
					value >>= 1;
			}
			table[i] = value;
		}
	}

	U32 operator()(U32 crc, void *buffer, std::streamsize count) {
		U8 *p = (U8*)buffer;
		U32 temp1, temp2;

		while (count-- != 0)
		{
			temp1 = (crc >> 8) & 0x00FFFFFFL;
			temp2 = table[(crc ^ *p++) & 0xff];
			crc = temp1 ^ temp2;
		}
		return crc;
	}

} crc32;
        
/* Fast log2 of an integer with a lookup table
http://graphics.stanford.edu/~seander/bithacks.html#IntegerLogLookup */
unsigned logi(unsigned v) {
	static const char LogTable256[256] = {
#define LT(n) n, n, n, n, n, n, n, n, n, n, n, n, n, n, n, n
		- 1, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
		LT(4), LT(5), LT(5), LT(6), LT(6), LT(6), LT(6),
		LT(7), LT(7), LT(7), LT(7), LT(7), LT(7), LT(7), LT(7)
	};
	register unsigned int t, tt; // temporaries
	if (tt = v >> 16)
		return (t = tt >> 8) ? 24 + LogTable256[t] : 16 + LogTable256[tt];
	else
		return (t = v >> 8) ? 8 + LogTable256[t] : LogTable256[v];
}

#if defined (SIMD)
template <class T> void alloc(T*&p, int n) {
	const size_t sz = n*sizeof(T);
#ifdef __GNUC__
    posix_memalign((void **)&p, 16, sz);
#else
	p = (T*)aligned_alloc(16, sz);
#endif
	if (!p) printf("Out of memory\n"), exit(1);
	memset(p, 0, sz);
}

template <class T> void dealloc(T*&p) {
	aligned_free(p);
	p = NULL;
}
#else
// Create an array p of n elements of type T
template <class T> void alloc(T*&p, int n) {
	p=(T*)calloc(n, sizeof(T));
	if (!p) printf("Out of memory\n"), exit(1);
}

template <class T> void dealloc(T*&p) {
	free(p);
	p = NULL;
}
#endif


/*** Debug helpers ***/
#if defined (SIMD) && defined(DEBUG)
void print128_epi16(__m128i var)
{
    short *val = (short*) &var;
    printf("epi16: %i\t%i\t%i\t%i\t%i\t%i\t%i\t%i\n",
           val[0], val[1], val[2], val[3], val[4], val[5],
           val[6], val[7]);
}

void print128_epi32(__m128i var)
{
    int *val = (int*) &var;
    printf("epi32: %i\t%i\t%i\t%i\t\n",
           val[0], val[1], val[2], val[3]);
}
#endif

/*** Matt Mahoney paq8f fragment ***/
static const U8 State_table[256][4]={
    {  1,  2, 0, 0},{  3,  5, 1, 0},{  4,  6, 0, 1},{  7, 10, 2, 0}, // 0-3
    {  8, 12, 1, 1},{  9, 13, 1, 1},{ 11, 14, 0, 2},{ 15, 19, 3, 0}, // 4-7
    { 16, 23, 2, 1},{ 17, 24, 2, 1},{ 18, 25, 2, 1},{ 20, 27, 1, 2}, // 8-11
    { 21, 28, 1, 2},{ 22, 29, 1, 2},{ 26, 30, 0, 3},{ 31, 33, 4, 0}, // 12-15
    { 32, 35, 3, 1},{ 32, 35, 3, 1},{ 32, 35, 3, 1},{ 32, 35, 3, 1}, // 16-19
    { 34, 37, 2, 2},{ 34, 37, 2, 2},{ 34, 37, 2, 2},{ 34, 37, 2, 2}, // 20-23
    { 34, 37, 2, 2},{ 34, 37, 2, 2},{ 36, 39, 1, 3},{ 36, 39, 1, 3}, // 24-27
    { 36, 39, 1, 3},{ 36, 39, 1, 3},{ 38, 40, 0, 4},{ 41, 43, 5, 0}, // 28-31
    { 42, 45, 4, 1},{ 42, 45, 4, 1},{ 44, 47, 3, 2},{ 44, 47, 3, 2}, // 32-35
    { 46, 49, 2, 3},{ 46, 49, 2, 3},{ 48, 51, 1, 4},{ 48, 51, 1, 4}, // 36-39
    { 50, 52, 0, 5},{ 53, 43, 6, 0},{ 54, 57, 5, 1},{ 54, 57, 5, 1}, // 40-43
    { 56, 59, 4, 2},{ 56, 59, 4, 2},{ 58, 61, 3, 3},{ 58, 61, 3, 3}, // 44-47
    { 60, 63, 2, 4},{ 60, 63, 2, 4},{ 62, 65, 1, 5},{ 62, 65, 1, 5}, // 48-51
    { 50, 66, 0, 6},{ 67, 55, 7, 0},{ 68, 57, 6, 1},{ 68, 57, 6, 1}, // 52-55
    { 70, 73, 5, 2},{ 70, 73, 5, 2},{ 72, 75, 4, 3},{ 72, 75, 4, 3}, // 56-59
    { 74, 77, 3, 4},{ 74, 77, 3, 4},{ 76, 79, 2, 5},{ 76, 79, 2, 5}, // 60-63
    { 62, 81, 1, 6},{ 62, 81, 1, 6},{ 64, 82, 0, 7},{ 83, 69, 8, 0}, // 64-67
    { 84, 71, 7, 1},{ 84, 71, 7, 1},{ 86, 73, 6, 2},{ 86, 73, 6, 2}, // 68-71
    { 44, 59, 5, 3},{ 44, 59, 5, 3},{ 58, 61, 4, 4},{ 58, 61, 4, 4}, // 72-75
    { 60, 49, 3, 5},{ 60, 49, 3, 5},{ 76, 89, 2, 6},{ 76, 89, 2, 6}, // 76-79
    { 78, 91, 1, 7},{ 78, 91, 1, 7},{ 80, 92, 0, 8},{ 93, 69, 9, 0}, // 80-83
    { 94, 87, 8, 1},{ 94, 87, 8, 1},{ 96, 45, 7, 2},{ 96, 45, 7, 2}, // 84-87
    { 48, 99, 2, 7},{ 48, 99, 2, 7},{ 88,101, 1, 8},{ 88,101, 1, 8}, // 88-91
    { 80,102, 0, 9},{103, 69,10, 0},{104, 87, 9, 1},{104, 87, 9, 1}, // 92-95
    {106, 57, 8, 2},{106, 57, 8, 2},{ 62,109, 2, 8},{ 62,109, 2, 8}, // 96-99
    { 88,111, 1, 9},{ 88,111, 1, 9},{ 80,112, 0,10},{113, 85,11, 0}, // 100-103
    {114, 87,10, 1},{114, 87,10, 1},{116, 57, 9, 2},{116, 57, 9, 2}, // 104-107
    { 62,119, 2, 9},{ 62,119, 2, 9},{ 88,121, 1,10},{ 88,121, 1,10}, // 108-111
    { 90,122, 0,11},{123, 85,12, 0},{124, 97,11, 1},{124, 97,11, 1}, // 112-115
    {126, 57,10, 2},{126, 57,10, 2},{ 62,129, 2,10},{ 62,129, 2,10}, // 116-119
    { 98,131, 1,11},{ 98,131, 1,11},{ 90,132, 0,12},{133, 85,13, 0}, // 120-123
    {134, 97,12, 1},{134, 97,12, 1},{136, 57,11, 2},{136, 57,11, 2}, // 124-127
    { 62,139, 2,11},{ 62,139, 2,11},{ 98,141, 1,12},{ 98,141, 1,12}, // 128-131
    { 90,142, 0,13},{143, 95,14, 0},{144, 97,13, 1},{144, 97,13, 1}, // 132-135
    { 68, 57,12, 2},{ 68, 57,12, 2},{ 62, 81, 2,12},{ 62, 81, 2,12}, // 136-139
    { 98,147, 1,13},{ 98,147, 1,13},{100,148, 0,14},{149, 95,15, 0}, // 140-143
    {150,107,14, 1},{150,107,14, 1},{108,151, 1,14},{108,151, 1,14}, // 144-147
    {100,152, 0,15},{153, 95,16, 0},{154,107,15, 1},{108,155, 1,15}, // 148-151
    {100,156, 0,16},{157, 95,17, 0},{158,107,16, 1},{108,159, 1,16}, // 152-155
    {100,160, 0,17},{161,105,18, 0},{162,107,17, 1},{108,163, 1,17}, // 156-159
    {110,164, 0,18},{165,105,19, 0},{166,117,18, 1},{118,167, 1,18}, // 160-163
    {110,168, 0,19},{169,105,20, 0},{170,117,19, 1},{118,171, 1,19}, // 164-167
    {110,172, 0,20},{173,105,21, 0},{174,117,20, 1},{118,175, 1,20}, // 168-171
    {110,176, 0,21},{177,105,22, 0},{178,117,21, 1},{118,179, 1,21}, // 172-175
    {110,180, 0,22},{181,115,23, 0},{182,117,22, 1},{118,183, 1,22}, // 176-179
    {120,184, 0,23},{185,115,24, 0},{186,127,23, 1},{128,187, 1,23}, // 180-183
    {120,188, 0,24},{189,115,25, 0},{190,127,24, 1},{128,191, 1,24}, // 184-187
    {120,192, 0,25},{193,115,26, 0},{194,127,25, 1},{128,195, 1,25}, // 188-191
    {120,196, 0,26},{197,115,27, 0},{198,127,26, 1},{128,199, 1,26}, // 192-195
    {120,200, 0,27},{201,115,28, 0},{202,127,27, 1},{128,203, 1,27}, // 196-199
    {120,204, 0,28},{205,115,29, 0},{206,127,28, 1},{128,207, 1,28}, // 200-203
    {120,208, 0,29},{209,125,30, 0},{210,127,29, 1},{128,211, 1,29}, // 204-207
    {130,212, 0,30},{213,125,31, 0},{214,137,30, 1},{138,215, 1,30}, // 208-211
    {130,216, 0,31},{217,125,32, 0},{218,137,31, 1},{138,219, 1,31}, // 212-215
    {130,220, 0,32},{221,125,33, 0},{222,137,32, 1},{138,223, 1,32}, // 216-219
    {130,224, 0,33},{225,125,34, 0},{226,137,33, 1},{138,227, 1,33}, // 220-223
    {130,228, 0,34},{229,125,35, 0},{230,137,34, 1},{138,231, 1,34}, // 224-227
    {130,232, 0,35},{233,125,36, 0},{234,137,35, 1},{138,235, 1,35}, // 228-231
    {130,236, 0,36},{237,125,37, 0},{238,137,36, 1},{138,239, 1,36}, // 232-235
    {130,240, 0,37},{241,125,38, 0},{242,137,37, 1},{138,243, 1,37}, // 236-239
    {130,244, 0,38},{245,135,39, 0},{246,137,38, 1},{138,247, 1,38}, // 240-243
    {140,248, 0,39},{249,135,40, 0},{250, 69,39, 1},{ 80,251, 1,39}, // 244-247
    {140,252, 0,40},{249,135,41, 0},{250, 69,40, 1},{ 80,251, 1,40}, // 248-251
    {140,252, 0,41}};  // 252, 253-255 are reserved

#define nex(state,sel) State_table[state][sel]

///////////////////////////// Squash //////////////////////////////
// return p = 1/(1 + exp(-d)), d scaled by 8 bits, p scaled by 12 bits
class Squash {
	short tab[4096];
public:
	Squash();
	int operator()(int d) {
		d += 2048;
		if (d<0) return 0;
		else if (d>4095) return 4095;
		else return tab[d];
	}
} squash;

Squash::Squash() {
	static const int t[33] = {
		1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101,
		1546, 2047, 2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022,
		4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094 };
	for (int i = -2048; i<2048; ++i) {
		int w = i & 127;
		int d = (i >> 7) + 16;
		tab[i + 2048] = (t[d] * (128 - w) + t[(d + 1)] * w + 64) >> 7;
	}
}

//////////////////////////// Stretch ///////////////////////////////

// Inverse of squash. stretch(d) returns ln(p/(1-p)), d scaled by 8 bits,
// p by 12 bits.  d has range -2047 to 2047 representing -8 to 8.  
// p has range 0 to 4095 representing 0 to 1.

class Stretch {
	short t[4096];
public:
	Stretch();
	int operator()(int p) const {
		assert(p >= 0 && p<4096);
		return t[p];
	}
} stretch;

Stretch::Stretch() {
	int pi = 0;
	for (int x = -2047; x <= 2047; ++x) {  // invert squash()
		int i = squash(x);
		for (int j = pi; j <= i; ++j)
			t[j] = x;
		pi = i + 1;
	}
	t[4095] = 2047;
}

extern int level;  // Compression level 0 to 9
#define MEM (0x10000<<level)

//////////////////////////// Mix, APM /////////////////////////

// Mix combines 2 predictions and a context to produce a new prediction.
// Methods:
// Mix m(n) -- creates allowing with n contexts.
// m.pp(p1, p2, cx) -- inputs 2 stretched predictions and a context cx
//   (0..n-1) and returns a stretched prediction.  Stretched predictions
//   are fixed point numbers with an 8 bit fraction, normally -2047..2047
//   representing -8..8, such that 1/(1+exp(-p) is the probability that
//   the next update will be 1.
// m.update(y) updates the model after a prediction with bit y (0..1).

class Mix {
protected:
	const int N;  // n
	int* wt;  // weights, scaled 24 bits
	int x1, x2;    // inputs, scaled 8 bits (-2047 to 2047)
	int cxt;  // last context (0..n-1)
	int pr;   // last output
public:
	Mix(int n = 512);
	int pp(int p1, int p2, int cx) {
		assert(cx >= 0 && cx<N);
		cxt = cx * 2;
		return pr = (x1 = p1)*(wt[cxt] >> 16) + (x2 = p2)*(wt[cxt + 1] >> 16) + 128 >> 8;
	}
	void update(int y) {
		assert(y == 0 || y == 1);
		int err = ((y << 12) - squash(pr));
		if ((wt[cxt] & 3)<3)
			err *= 4 - (++wt[cxt] & 3);
		err = err + 8 >> 4;
		wt[cxt] += x1*err&-4;
		wt[cxt + 1] += x2*err;
	}
};

Mix::Mix(int n) : N(n), x1(0), x2(0), cxt(0), pr(0) {
	alloc(wt, n * 2);
	for (int i = 0; i<N * 2; ++i)
		wt[i] = 1 << 23;
}

// An APM is a Mix optimized for a constant in place of p1, used to
// refine a stretched prediction given a context cx. 
// Normally p1 is in the range (0..4095) and p2 is doubled.

class APM : public Mix {
public:
	APM(int n);
};

APM::APM(int n): Mix(n) {
	for (int i=0; i<n; ++i)
		wt[2*i]=0;
}

// A StateMap maps a context to a probability.  Methods:
//
// Statemap sm(n) creates a StateMap with n contexts using 4*n bytes memory.
// sm.p(cx, limit) converts state cx (0..n-1) to a probability (0..4095)
//     that the next updated bit y=1.
//     limit (1..1023, default 255) is the maximum count for computing a
//     prediction.  Larger values are better for stationary sources.
// sm.update(y) updates the model with actual bit y (0..1).

class StateMap {
protected:
	const int N;  // Number of contexts
	int cxt;      // Context of last prediction
	U32 *t;       // cxt -> prediction in high 22 bits, count in low 10 bits
	static int dt[1024];  // i -> 16K/(i+3)
public:
	StateMap(int n = 256);

	// update bit y (0..1)
	void update(int y, int limit = 255) {
		assert(cxt >= 0 && cxt<N);
		int n = t[cxt] & 1023, p = t[cxt] >> 10;  // count, prediction
		if (n<limit) ++t[cxt];
		else t[cxt] = t[cxt] & 0xfffffc00 | limit;
		t[cxt] += (((y << 22) - p) >> 3)*dt[n] & 0xfffffc00;
	}

	// predict next bit in context cx
	int p(int cx) {
		assert(cx >= 0 && cx<N);
		return t[cxt = cx] >> 20;
	}
};

int StateMap::dt[1024] = { 0 };

StateMap::StateMap(int n) : N(n), cxt(0) {
	alloc(t, N);
	for (int i = 0; i<N; ++i)
		t[i] = 1 << 31;
	if (dt[0] == 0)
		for (int i = 0; i<1024; ++i)
			dt[i] = 16384 / (i + i + 3);
}

// Hash 2-5 ints.
inline U32 hash(U32 a, U32 b, U32 c=0xffffffff, U32 d=0xffffffff,
				U32 e=0xffffffff) {
    U32 h=a*200002979u+b*30005491u+c*50004239u+d*70004807u+e*110002499u;
    return h^h>>9^a>>2^b>>3^c>>4^d>>5^e>>6;
}

/* Dynamic Markov Modelling G.V. CORMACK AND R. N. S. HORSPOOL, 1986 */

struct DMCNode {  // 12 bytes
    unsigned int nx[2];  // next pointers
    U8 state;  // bit history
    unsigned int c0:12, c1:12;  // counts * 256
};

class dmcModel {
private:
	const int N;
    int top=0, curr=0;  // allocated, current node
    DMCNode *t;			// state graph
    StateMap sm;
    int threshold=256;
	Mix mx;
    
public:
    dmcModel(int n, int cx_sz) : N(n), mx(cx_sz) {
		alloc(t, N);
    }

	~dmcModel() {
		dealloc(t);
	}

	void reset() {
		curr = 0;
	}

	void update(int y) {
		// clone next state
		if (top>0 && top<N) {
			int next = t[curr].nx[y];
			int n = y ? t[curr].c1 : t[curr].c0;
			int nn = t[next].c0 + t[next].c1;
			if (n >= threshold * 2 && nn - n >= threshold * 3) {
				int r = n * 4096 / nn;
				assert(r >= 0 && r <= 4096);
				t[next].c0 -= t[top].c0 = t[next].c0*r >> 12;
				t[next].c1 -= t[top].c1 = t[next].c1*r >> 12;
				t[top].nx[0] = t[next].nx[0];
				t[top].nx[1] = t[next].nx[1];
				t[top].state = t[next].state;
				t[curr].nx[y] = top;
				++top;
				if (top == MEM * 2) threshold = 512;
				if (top == MEM * 3) threshold = 768;
			}
		}
		// update count, state
		if (y) {
			if (t[curr].c1<3800) t[curr].c1 += 256;
		}
		else {
			if (t[curr].c0<3800) t[curr].c0 += 256;
		}
		curr = t[curr].nx[y];
		t[curr].state = nex(t[curr].state, y);
		
		sm.update(y);
		mx.update(y);
	}
    
    int operator()(int cxt) {        
        
		// Initialize to a bytewise order 1 model at startup or when flushing memory
		if (top==N) top=0;
        if (top==0) {
            assert(N>=65536);
            for (int i=0; i<256; ++i) {
                for (int j=0; j<256; ++j) {
                    if (i<127) {
                        t[j*256+i].nx[0]=j*256+i*2+1;
                        t[j*256+i].nx[1]=j*256+i*2+2;
                    }
                    else {
                        t[j*256+i].nx[0]=(i-127)*256;
                        t[j*256+i].nx[1]=(i+1)*256;
                    }
                    t[j*256+i].c0=128;
                    t[j*256+i].c1=128;
                }
            }
            top=65536;
            curr=0;
            threshold=256;
        }
                        
        // predict
		const int pr1 = sm.p(t[curr].state);
		const int n1 = t[curr].c1;
		const int n0 = t[curr].c0;
		const int pr2 = (n1 + 5) * 4096 / (n0 + n1 + 10);
		return mx.pp(stretch(pr1), stretch(pr2), cxt);
    }
};

// Unlike lpaq we do not use the deep context as it does not bring any benefit
class Predictor
{
private:
	const int ssize;
    const int sampleSize;
    const int numChannels;
	APM a1,a2,a3;
    dmcModel dmc;
	int cxt;
	int c0 = 1;				// Last 0-7 bits of the partial byte with a leading 1 bit (1-255)
	U32 c4 = 0;				// Last 4 whole bytes, packed.  Last byte is bits 0-7.
	std::vector<int> h;

public:
	Predictor(int sampleSize_, int numChannels_):
        ssize(sampleSize_*numChannels_),
        sampleSize(sampleSize_), numChannels(numChannels_),
        a1(ssize),
        a2(0x10000),
        a3(0x10000),
        h(ssize),
		dmc(MEM*5, ssize),
		cxt(0) {
	}
    
	unsigned int P() {
		int pr = dmc(cxt);
		pr = a1.pp(512, pr * 2, cxt) * 3 + pr >> 2;  // Adjust prediction
		pr = a2.pp(512, pr * 2, h[cxt] & 0xffff) * 3 + pr >> 2;
		pr = a3.pp(512, pr * 2, h[cxt] << 8 & 0xff00 | c0 & 255) * 3 + pr >> 2;
		return squash(pr);
 	}

	void reset()
	{		
		//cxt = ((cxt / sampleSize + 1) * sampleSize) % ssize;
		dmc.reset();
	}

	void rollback()
	{
		cxt = 0;
	}
  
    void update(int y) {       	
		c0 += c0 + y;
		if (c0 >= 256) {
			c4 = (c4 << 8) + c0 - 256;
			c0 = 1;
		}
		cxt = (cxt + 1) % ssize;
		h[cxt] += h[cxt] + y;

		dmc.update(y);

		a1.update(y);
		a2.update(y);
		a3.update(y);
	}
    
};

class BinaryEncoder
{
private:
	std::ofstream &_out;
	U32 _x1;
	U32 _x2;
	U32 _x;
	U32 _numBytes;

	void write(unsigned char c) {
		_out.write((const char *)&c, 1);
		_numBytes++;
	}
   
public:
	BinaryEncoder(std::ofstream &out)
    : _out(out), _x1(0), _x2(0xffffffff), _x(0), _numBytes(0) { }
    
	void writeBit(int b, Predictor &pr)
	{
		unsigned int p = pr.P();
		p+=p<2048;
        
		unsigned int xmid = _x1 + (_x2-_x1>>12)*p + ((_x2-_x1&0xfff)*p>>12);
		assert (xmid >= _x1 && xmid < _x2);
        
		if (b)
			_x2 = xmid;
		else
			_x1 = xmid + 1;
        
		pr.update (b);
        
		// Shift equal MSB's out
		while (((_x1 ^ _x2) & 0xff000000) == 0) {
			write(_x2 >> 24);					
			_x1 <<= 8;
			_x2 = (_x2 << 8) + 255;			
		}
	}
    
	void write(unsigned c, int nbits, Predictor &pr)
	{
		for (int i = nbits -1; i >= 0; i--) {
			writeBit ((c >> i) & 1, pr);
		}
	}
    
	void flush() {		
		write(_x1 >> 24);  
	}	

	int getNumBytes()
	{
		return _numBytes;
	}

};

class BinaryDecoder
{
private:
	std::ifstream &_in;
	U32 _x1;
	U32 _x2;
	U32 _x;
    
public:
	BinaryDecoder(std::ifstream &in)
    : _in(in), _x1(0), _x2(0xffffffff), _x(0)
	{
		for (int i = 1; i <= 4; i++) {
			int c = 0;
			if (!_in.eof())
				_in.read((char *)&c, 1);
			_x = (_x << 8) + (c & 0xFF);
		}
	}
    
	int readBit(Predictor &pr)
	{
		unsigned int p = pr.P();
		p+=p<2048;
		unsigned int xmid = _x1 + (_x2-_x1>>12)*p + ((_x2-_x1&0xfff)*p>>12);
        
		assert(xmid >= _x1 && xmid < _x2);
        
		int b = 0;
		if (_x <= xmid) {
			b = 1;
			_x2 = xmid;
		} else
			_x1 = xmid + 1;
        
		pr.update (b);
        
		// Shift equal MSB's out
		while (((_x1^_x2)&0xff000000)==0) {
			_x1 <<= 8;
			_x2 = (_x2 << 8) + 255;
			int c = 0;
			if (!_in.eof())
				_in.read((char *)&c, 1);
			_x = (_x << 8) + (c & 0xFF);
		}
        
		return b;
	}
    
	unsigned read(Predictor &pr, int numbits)
	{
		unsigned res = 0;
		for (int i = 0; i < numbits; i++) {
			res = (res << 1) | readBit(pr);
		}
		return res;
	}	
};

class NNFilter {
    
private:
    
    const int N;
    const int SZ;
    const int shift;
    
	int *x;
	short *w;
	short *dx;
    
    int bp;
    int avg;
    
    void adjust()
    {
        bp++;
        if (bp == SZ) {
            bp = N;
            for(int i = -N; i < 0; i++) {
                x[bp + i] = x[SZ + i];
                dx[bp + i] = dx[SZ + i];
            }
        }
    }
    
    LONG product(short *w, int *x) {
        LONG p = 0;
#ifdef SIMD
        int j = 0;
        __m128i sseX, sseW, sseW1, sseW2, sseDotProduct;
        __m128i sseSum = _mm_setzero_si128();
        while (j < N) { // _mm_loadu_si128
            sseW = _mm_lddqu_si128((__m128i *)&w[j]);
            sseW1 = _mm_srai_epi32(_mm_unpacklo_epi16(sseW, sseW), 16);
            sseW2 = _mm_srai_epi32(_mm_unpackhi_epi16(sseW, sseW), 16);
            
            sseX = _mm_lddqu_si128((__m128i *)&x[j]);
            sseDotProduct = _mm_mullo_epi32(sseW1, sseX);
            sseSum = _mm_add_epi32(sseSum, sseDotProduct);
            
            sseX = _mm_lddqu_si128((__m128i *)&x[j + 4]);
            sseDotProduct = _mm_mullo_epi32(sseW2, sseX);
            sseSum = _mm_add_epi32(sseSum, sseDotProduct);

            j += 8;
        }
        sseSum = _mm_add_epi32(sseSum, _mm_srli_si128(sseSum, 8));
        sseSum = _mm_add_epi32(sseSum, _mm_srli_si128(sseSum, 4));
        p = _mm_cvtsi128_si32(sseSum);
#else
        for (int j = 0; j < N; j++) {
            p += w[j] * x[j];
        }
#endif
        return p;
    }
    
    void adapt(short *w, short *dx, int err) {
#ifdef SIMD
        if (err != 0) {
            if (err < 0) {
                for (size_t z = 0; z < N; z += 8) {
                    __m128i sseM = _mm_loadu_si128((__m128i *)&w[z]);
                    __m128i sseAdapt = _mm_loadu_si128((__m128i *)&dx[z]);
                    __m128i sseNew = _mm_add_epi16(sseM, sseAdapt);
                    _mm_storeu_si128((__m128i *)&w[z], sseNew);
                }
            } else {
                for (size_t z = 0; z < N; z += 8) {
                    __m128i sseM = _mm_loadu_si128((__m128i *)&w[z]);
                    __m128i sseAdapt = _mm_loadu_si128((__m128i *)&dx[z]);
                    __m128i sseNew = _mm_sub_epi16(sseM, sseAdapt);
                    _mm_storeu_si128((__m128i *)&w[z], sseNew);
                }
            }
        }
#else
        if (err < 0) {
            for (int j = 0; j < N; j++)
                w[j] += dx[j];
        } else {
            for (int j = 0; j < N; j++)
                w[j] -= dx[j];
        }
#endif
    }
    
public:
    NNFilter(int n, int shift_) :
        N(n), SZ(NN_WINDOW_SIZE + n), shift(shift_), bp(n), avg(0) {
        assert (!((n <= 0) || ((n % 16) != 0)));
		alloc(x, SZ);
		alloc(dx, SZ);
		alloc(w, N);
    }

	~NNFilter() {
		dealloc(x);
		dealloc(dx);
		dealloc(w);
	}
    
    void encode(int *diff) {
        int inn = *diff;
        x[bp] = inn;
        LONG pr = product(&w[0], &x[bp-N]);
        *diff -= (int)QUANTIZE64(pr, shift);
        adapt(&w[0], &dx[bp-N], *diff);
        int inAbs = abs(inn);
        if (inAbs > (avg * 3))
            dx[bp] = ((inn >> 25) & 64) - 32;
        else if (inAbs > (avg * 4) / 3)
            dx[bp] = ((inn >> 26) & 32) - 16;
        else if (inAbs > 0)
            dx[bp] = ((inn >> 27) & 16) - 8;
        else
            dx[bp] = 0;
        avg += (inAbs - avg) / 16;
        dx[bp-1] >>= 1;
        dx[bp-2] >>= 1;
        dx[bp-8] >>= 1;
        adjust();
    }
    
    void decode(int *diff) {
        LONG pr = product(&w[0], &x[bp-N]);
        adapt(&w[0], &dx[bp-N], *diff);
        *diff += (int)QUANTIZE64(pr, shift);
        x[bp] = *diff;
        int inAbs = abs(*diff);
        if (inAbs > (avg * 3))
            dx[bp] = ((*diff >> 25) & 64) - 32;
        else if (inAbs > (avg * 4) / 3)
            dx[bp] = ((*diff >> 26) & 32) - 16;
        else if (inAbs > 0)
            dx[bp] = ((*diff >> 27) & 16) - 8;
        else
            dx[bp] = 0;
        avg += (inAbs - avg) / 16;
        dx[bp-1] >>= 1;
        dx[bp-2] >>= 1;
        dx[bp-8] >>= 1;
        adjust();
    }

};


class SFilter {
private:
    static const int M = 12;
    const FLOAT gamma = 0.007f;
	
    const int mu;
    const int shift;
 
    short *w;
    int *x;
    int pr;
	LONG norm;	

	double rate;
    int oldErr;
    short *pw;
    int wt;
    
    int dotProduct() {
#ifdef SIMD
        // 12 (short * int) = (8 x short) * 2 x (4 x int) + (4 x short) * (4 x int)
        __m128i sseX, sseW, sseW1, sseW2, sseDotProduct;
        __m128i sseSum = _mm_setzero_si128();
        
        sseW = _mm_load_si128((__m128i *)&w[0]); // 8 x short
        sseW1 = _mm_srai_epi32(_mm_unpacklo_epi16(sseW, sseW), 16);
        sseW2 = _mm_srai_epi32(_mm_unpackhi_epi16(sseW, sseW), 16);
        
        sseX = _mm_load_si128((__m128i *)&x[0]); // 4 x int
        sseDotProduct = _mm_mullo_epi32(sseW1, sseX);
        sseSum = _mm_add_epi32(sseSum, sseDotProduct);
        
        sseX = _mm_load_si128((__m128i *)&x[4]); // 4 x int
        sseDotProduct = _mm_mullo_epi32(sseW2, sseX);
        sseSum = _mm_add_epi32(sseSum, sseDotProduct);
        
        sseW = _mm_loadl_epi64((__m128i *)&w[8]); // 4 x short
        sseW1 = _mm_srai_epi32(_mm_unpacklo_epi16(sseW, sseW), 16);
        
        sseX = _mm_load_si128((__m128i *)&x[8]);  // 4 x int
        sseDotProduct = _mm_mullo_epi32(sseW1, sseX);
        sseSum = _mm_add_epi32(sseSum, sseDotProduct);
        
        sseSum = _mm_add_epi32(sseSum, _mm_srli_si128(sseSum, 8));
        sseSum = _mm_add_epi32(sseSum, _mm_srli_si128(sseSum, 4));
        
        return _mm_cvtsi128_si32(sseSum);
#else
        int sum = 0;
        for (int j = 0; j < M; j++) {
            sum += w[j] * x[j];
        }
		return sum;
#endif
    }    

	int draw() {
		const double naf = (1 - std::numeric_limits<double>::epsilon()) / 2;
		const double thr = std::numeric_limits<int>::max() + naf;
		double num;
		do {
			num = std::floor(std::log(1.0f - xrnd()) / rate);
		} while (num >= thr);
		int rt = (int)(num + naf);
		rt = (rt < M) ? rt : M - 1;
		return rt;
	}
    
public:
    SFilter(int mu_ = 6, int shift_ = 11) : mu(mu_), shift(shift_), pr(0), norm(0), pw(nullptr) {
        alloc(w, M);
        alloc(x, M);

		rate = log(1 - 1.0/M);
    }
    
    ~SFilter() {
        dealloc(w);
        dealloc(x);
    }
    
    int predict() {
        pr = QUANTIZE(dotProduct(), shift);
        return pr;
    }
    
    void update(int in) {               
        int Err = in - pr;
        
        if (pw != nullptr) { // Metropolis-Hastings 
            FLOAT p = Err != 0 ? (FLOAT)oldErr / Err : 1.0f;
            if (p <= 1.0f && xrnd() > p) { // reject
                *pw -= wt;
            }
        }
        
		if (Err != 0 && norm > 0) { // Stochastic gradient descent with normalised least mean squares step 
			int j = draw();         // Draw sample with geometric distribution
            pw = &w[j];
			LONG wx = 2L * Err * x[j];
			LONG div = (mu * (norm >> 7));
			if (div == 0) div = 1;
			wt = (int)(wx / div);
			if (wx != 0 && wt == 0 && xrnd() < 0.2) { // Mutation
 				if (wx > 0)
					wt = 1;
				else if (wx < 0)
					wt = -1;
			}
            *pw += wt;
		}

        oldErr = Err;
		norm -= (LONG)x[M - 1] * (LONG)x[M - 1];
		norm += (LONG)in * (LONG)in;
        memmove(&x[1], &x[0], (M - 1) * sizeof(int));
		x[0] = in;
    }
    
    void encode(int *in) {
        int inn = *in;
        *in -= predict();
        update(inn);
    }

	void decode(int *in) {
		*in += predict();
		update(*in);
	}	
};

std::string getfilename(const std::string& str) {
	const char *path = str.c_str();
#if defined(_WIN32)
	char fname[_MAX_FNAME];
	char ext[_MAX_EXT];
	_splitpath(path, NULL, NULL, fname, ext);
	std::string res = std::string(fname).append(ext);
#else
	std::string res = std::string(basename(path));
#endif
	return res;
}

U32 getFileCrc32(std::ifstream& ifs) {
	ifs.seekg(0);
	U32 res = 0;
	char buffer[CHAR_BUFFER_SIZE];
	while (!ifs.eof()) {
		ifs.read(buffer, sizeof(buffer));
		res = crc32(res, buffer, ifs.gcount());
	}
	return res;
}

void chunkh(std::ifstream& ifs, Chunk &chunk)
{
	chunk.beg = ifs.tellg();

	ifs.read((char *)&chunk.chkID, sizeof(ID));
	ifs.read((char *)&chunk.ckSize, sizeof(chunk.ckSize));

	chunk.chkID = BINARY_SWAP32(chunk.chkID);
	chunk.ckSize = BINARY_SWAP32(chunk.ckSize);
}

void chunkw(std::ofstream &ofs, Chunk &chunk) {
	
	chunk.beg = ofs.tellp();

	ID chkID = BINARY_SWAP32(chunk.chkID);
	int ckSize = BINARY_SWAP32(chunk.ckSize);

	ofs.write((const char *)&chkID, sizeof(ID));
	ofs.write((const char *)&ckSize, sizeof(ckSize));
}

void chunkrw(std::ofstream &ofs, Chunk &chunk) {
	std::streampos cur = ofs.tellp();
	ofs.seekp(chunk.beg);
	chunkw(ofs, chunk);
	ofs.seekp(cur);
}

void chunkcp(std::ifstream& ifs, std::ofstream &ofs, Chunk &chunk)
{
	std::streamoff bookmark = ifs.tellg();
	char buffer[2048];
	size_t size = chunk.ckSize;
	while ( size > 0 )
	{
		size_t length = std::min(size, sizeof(buffer));
		ifs.read(buffer, length);
		ofs.write(buffer, length);
		if (ifs.bad() || ofs.bad()) {
			break;
		}
		size -= length;
	} 
	
	ifs.seekg(bookmark);
}

#if defined(LOG)
#define LOGCHUNK(x) logchunk(x)
void logchunk(Chunk &chunk)
{
	char buffer[5];
	ID chkID = BINARY_SWAP32(chunk.chkID);
	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer, &chkID, sizeof(ID));
	COUT << "* Chunk " << buffer << " size " <<
		chunk.ckSize << " at " << chunk.beg << std::endl;
}
#else
#define LOGCHUNK(x)
#endif

inline int stream_read(std::ifstream& ifs, int sampleSize)
{
	int s = 0;
	char *ptr = (char *)&s;
	int numBytes = sampleSize / 8;
	ifs.read(ptr + sizeof(s) - numBytes, numBytes);
	s = BINARY_SWAP32(s) & ((1U << sampleSize) -1);
	int mask = 1U << (sampleSize -1);
	return (s ^ mask) - mask;
}

inline void stream_write(std::ofstream& ofs, int sampleSize, int sample)
{	
	int mask = 1U << (sampleSize -1);
	int s = (sample + mask) ^ mask;
	s = BINARY_SWAP32(s);
	char *ptr = (char *)&s;
	int numBytes = sampleSize / 8;
	ofs.write(ptr + sizeof(s) - numBytes, numBytes);
}


void readComm(std::ifstream& ifs, CommonChunk &comm)
{
	std::streamoff bookmark = ifs.tellg();
    
	ifs.read((char *)&comm.numChannels, sizeof(comm.numChannels));
	ifs.read((char *)&comm.numSampleFrames,
             sizeof(comm.numSampleFrames));
	ifs.read((char *)&comm.sampleSize, sizeof(comm.sampleSize));
        
	Extended sampleRate;
	ifs.read((char *)&sampleRate, sizeof(sampleRate));

	comm.numChannels = BINARY_SWAP16(comm.numChannels);
	comm.numSampleFrames = BINARY_SWAP32(comm.numSampleFrames);
	comm.sampleSize = BINARY_SWAP16(comm.sampleSize);

	// Swap 1st <-> 4th and 2nd <-> 3rd bytes
	U8 *ptr = sampleRate + 2;
	U8 val = *(ptr);
	*(ptr) = *(ptr + 3);
	*(ptr + 3) = val;
	ptr += 1;
	val = *(ptr);
	*(ptr) = *(ptr + 1);
	*(ptr + 1) = val;

	U32 last = 0;
	U32 mantissa = *((U32 *)(sampleRate + 2));
	U8 exp = 30 - *(sampleRate + 1);
	while (exp--)
	{
		last = mantissa;
		mantissa >>= 1;
	}
	if (last & 0x00000001)
		mantissa++;
	comm.sampleRate = mantissa;

	ifs.seekg(bookmark);
}

void ssndDump(std::ifstream& ifs, std::ofstream& ofs, int numChannels, int sampleSize, long numSampleFrames) {
	std::streamoff bookmark = ifs.tellg();
    
	unsigned offset = 0;
	unsigned blocksize = 0;
    
	ifs.read((char *)&offset, sizeof(offset));
	ifs.read((char *)&blocksize, sizeof(blocksize));
    
	offset = BINARY_SWAP32(offset);
	blocksize = BINARY_SWAP32(blocksize);
    
	ifs.seekg(offset, ifs.cur);
    
	COUT << "Dump SSND";
	int numBytes = sampleSize / 8;
	for (int k = 0; k < numSampleFrames; k++) {
		for (int s = 0; s < numChannels; s++) {
			long sample = 0;
			char *ptr = (char *)&sample;
			ifs.read(ptr + sizeof(sample) - numBytes, numBytes);
			ofs.write(ptr + sizeof(sample) - numBytes, numBytes);
		}
		if (k % 100000 == 0) {
			COUT << ".";
		}
	}
    
	COUT << std::endl;
	ifs.seekg(bookmark);
}

struct context {
public:
	SFilter *lms;
	NNFilter *f1;
	NNFilter *f2;

	context() {
		lms = nullptr;
		f1 = f2 = nullptr;
	}

	~context() {
		if (lms != nullptr) delete lms;
		if (f1 != nullptr) delete f1;
		if (f2 != nullptr) delete f2;
	}

	void prepare(short sampleSize);
};

void context::prepare(short sampleSize)
{	
	if (sampleSize == 8) {
		lms = new SFilter(6, 10);
		f1 = new NNFilter(256, 13);
		f2 = new NNFilter(32, 9);
	} else if (sampleSize == 16) {
		lms = new SFilter(6, 11);
		f1 = new NNFilter(256, 13);
		f2 = new NNFilter(32, 10);
	}
	else if (sampleSize == 24) {
		lms = new SFilter(6, 11);
		f1 = new NNFilter(256, 21);
		f2 = new NNFilter(32, 19);
	}
}

static int overflow_count = 0;

void write(int sample, int sampleSize, BinaryEncoder &encoder, Predictor &pr) {
	int rw = ENC(sample);
	if (rw < (1 << sampleSize) - 1) {
		encoder.write(rw, sampleSize, pr);
	} else { // overflow: signed number is longer than sample size
		encoder.write((1 << sampleSize) - 1, sampleSize, pr);
		encoder.write(rw, 32, pr);
		pr.rollback();
		overflow_count++;
	}
}

int read(int sampleSize, BinaryDecoder &decoder, Predictor &pr) {
	int rw = decoder.read(pr, sampleSize);
	if (rw == (1 << sampleSize) - 1) {
		rw = decoder.read(pr, 32);
		pr.rollback();
	}
	return DEC(rw);
}

std::streamoff ssndpack(std::ifstream& ifs, std::ofstream& ofs, short sampleSize, short numChannels, unsigned numSampleFrames) {

	std::streamoff bookmark = ifs.tellg();
	std::streamoff beg = ofs.tellp();

	unsigned offset = 0;
	unsigned blocksize = 0;

	ifs.read((char *)&offset, sizeof(offset));
	ifs.read((char *)&blocksize, sizeof(blocksize));

	ofs.write((char *)&blocksize, sizeof(blocksize));
	ofs.write((char *)&offset, sizeof(blocksize));

#if !defined (__BIG_ENDIAN__)
	offset = BINARY_SWAP32(offset);
	blocksize = BINARY_SWAP32(blocksize);
#endif	

	ifs.seekg(offset, ifs.cur);

	COUT << "Pack PCM...";

	BinaryEncoder encoder(ofs);
	Predictor pr(sampleSize, numChannels);
  	
	
	std::vector<context> chl(numChannels);
	for (int c = 0; c < numChannels; c++) {
		chl[c].prepare(sampleSize);
	}

	std::vector<int> sample[2];
	sample[0].resize(numChannels);
	sample[1].resize(numChannels);
	for (unsigned k = 0; k < numSampleFrames; k++) {
		for (int c = 0; c < numChannels; c++) {
			sample[0][c] = stream_read(ifs, sampleSize);
			OUTVAL(sample[0][c]);
		}
		if (numChannels == 2) {
			int dL = sample[0][0];
			int dR = sample[0][1];
			sample[0][0] = dL - dR;
			sample[0][1] = dR + sample[0][0] / 2;
		}
		for (int c = 0; c < numChannels; c++) {            
            int diff = sample[0][c] - sample[1][c];
            chl[c].lms->encode(&diff);
			if (chl[c].f1 != nullptr)
				chl[c].f1->encode(&diff);
			if (chl[c].f2 != nullptr)
				chl[c].f2->encode(&diff);
			write(diff, sampleSize, encoder, pr);
			pr.reset();
			OUTVAL(diff);           
		}		
		if ((k + 1) % 10000 == 0) {
			int dataSize = (sampleSize / 8) * numChannels * k;
			float ratio = (float)dataSize / std::max(encoder.getNumBytes(), 1);
			COUT.precision(5);
			COUT << "\rPack PCM... Completed:   " << int(float(k + 1) / numSampleFrames * 100) << "% (" << ratio << ")\t\t\t" << std::flush;
		}
		for (int c = 0; c < numChannels; c++) {
			sample[1][c] = sample[0][c];
		}	
		OUTENDL;
	}	
	encoder.write(0, sampleSize, pr);
	encoder.write(255, sampleSize, pr);
	encoder.write(0, sampleSize, pr);
	encoder.write(255, sampleSize, pr);
	encoder.flush();
	COUT << "\rPack PCM... DONE.\t\t\t\t\t\t" << std::endl;
	ifs.seekg(bookmark);

	return ofs.tellp() - beg;
}

std::streamoff ssndupack(std::ifstream& ifs, std::ofstream& ofs, short sampleSize, short numChannels, unsigned numSampleFrames) {
	
	std::streamoff bookmark = ifs.tellg();
	std::streamoff beg = ofs.tellp();

	unsigned offset = 0;
	unsigned blocksize = 0;

	ifs.read((char *)&offset, sizeof(offset));
	ifs.read((char *)&blocksize, sizeof(blocksize));

	ofs.write((char *)&blocksize, sizeof(blocksize));
	ofs.write((char *)&offset, sizeof(blocksize));

#if !defined (__BIG_ENDIAN__)
	offset = BINARY_SWAP32(offset);
	blocksize = BINARY_SWAP32(blocksize);
#endif

	while (offset-- > 0) {
		char ch = 0;
		ofs.write(&ch, 1);
	}

	COUT << "Unpack PCM...";
	
	BinaryDecoder decoder(ifs);
	Predictor pr(sampleSize, numChannels);

	std::vector<context> chl(numChannels);
	for (int c = 0; c < numChannels; c++) {
		chl[c].prepare(sampleSize);
	}

	std::vector<int> sample[2];
	sample[0].resize(numChannels);
	sample[1].resize(numChannels);
    
	for (unsigned k = 0; k < numSampleFrames; k++) {
		for (int c = 0; c < numChannels; c++) {
			int diff = read(sampleSize, decoder, pr);
			pr.reset();
			OUTVAL(diff);
			if (chl[c].f2 != nullptr)
				chl[c].f2->decode(&diff); 
			if (chl[c].f1 != nullptr)
				chl[c].f1->decode(&diff);
			chl[c].lms->decode(&diff);
			sample[0][c] = diff + sample[1][c];
		}
		for (int c = 0; c < numChannels; c++)
			sample[1][c] = sample[0][c];
		if (numChannels == 2) {
			int dL = sample[0][0];
			int dR = sample[0][1];
			sample[0][1] = dR - dL/2;
			sample[0][0] = dL + sample[0][1];
		}
		for (int c = 0; c < numChannels; c++) {
			stream_write(ofs, sampleSize, sample[0][c]);
			OUTVAL(sample[0][c]);
		}
		if ((k + 1) % 10000 == 0) {
			COUT << "\rUnpack PCM... Completed:  " << int(float(k + 1) / numSampleFrames * 100) << "% " << std::flush;
		}
		OUTENDL;
	}

	COUT << "\rUnpack PCM... DONE.\t\t\t\t\t\t" << std::endl;
	ifs.seekg(bookmark);

	return ofs.tellp() - beg;
}
        
int compressAIFF(const std::string &inputFile, const std::string &outputFile)
{			
	std::ifstream ifs (inputFile, std::ifstream::binary);
	if (!ifs.good()) {
		std::cerr << "Can't open input file " << inputFile << std::endl;
		return RETURN_CODE_IO_ERROR;
	}	
	COUT << "Encode file " << getfilename(inputFile) << std::endl;
	std::ofstream o(outputFile, std::ofstream::binary);
	if (!o.good()) {
		ifs.close();
		std::cerr << "Can't open output file " << outputFile << std::endl;
		return RETURN_CODE_IO_ERROR;
	}
	auto startTime = std::chrono::steady_clock::now();
	FileHeader hdr;
	hdr.id = 'HPAK';
	hdr.version = APP_VERSION;
	hdr.crc32 = 0;
	hdr.flags = (level & FileFlags::MEM_MASK) | FileFlags::AIFF;
	hdr.cbSize = 0;
	o.write((const char *)&hdr, sizeof(hdr)); // reserve space
	Chunk chunk;
	long ckTotalSize;
	chunkh(ifs, chunk);
	if (chunk.chkID != 'FORM')  {
		ifs.close();
		o.close();
		std::cerr << "Not AIFF input file." << std::endl;
		return RETURN_CODE_BAD_FILE;
	}	
	ckTotalSize = chunk.ckSize;
	ID formType;
	ifs.read((char *)&formType, sizeof(formType));
	formType = BINARY_SWAP32(formType);
	if (formType != 'AIFF')  {
		ifs.close();
		o.close();
		std::cerr << "Not AIFF input file." << std::endl;
		return RETURN_CODE_BAD_FILE;
	}
	LOGCHUNK(chunk);
	ckTotalSize -= 4;
	CommonChunk comm;
	comm.numChannels = 1;
	comm.sampleSize = 8;
	comm.numSampleFrames = 0;
	LONG dataSize = 0;
	LONG compressedSize = 0;	
	while (!ifs.eof() && ckTotalSize > 0) {
		chunkh(ifs, chunk);
		LOGCHUNK(chunk);
		chunkw(o, chunk);
		if (chunk.chkID == 'COMM') {
			comm.h = chunk;
			readComm(ifs, comm);
			chunkcp(ifs, o, chunk);
			if (comm.sampleSize > 24) {
				std::cerr << "Only 8, 16, 24 bit resolution file is supported." << std::endl;
				o.close();
				return RETURN_CODE_BAD_FILE;
			}
			std::ios::fmtflags fmt(COUT.flags());
			COUT << "Number Channels: " << comm.numChannels << std::endl;
			COUT << "Number of Sample Frames: " << comm.numSampleFrames << std::endl;
			COUT.precision(4);
			COUT << "Resolution: " << comm.sampleSize << " bit" << std::endl;
			COUT << "Frequency: " << comm.sampleRate / 1000.0 << " kHz" << std::endl;
			COUT << "Play time: " << (int)floor(comm.numSampleFrames / comm.sampleRate / 60) << " min " <<
				(int)floor(comm.numSampleFrames / comm.sampleRate) % 60 << " sec" << std::endl;
			COUT.flags(fmt);
		} 
		else if (chunk.chkID == 'SSND') {			
			Chunk newchunk = chunk;
			newchunk.ckSize = (int)ssndpack(ifs, o, comm.sampleSize, comm.numChannels, comm.numSampleFrames);
			dataSize += (comm.sampleSize / 8) * comm.numChannels * comm.numSampleFrames;
			compressedSize += newchunk.ckSize;
			chunkrw(o, newchunk);
		}
		else {			
			chunkcp(ifs, o, chunk);
		}		
		ifs.seekg(chunk.ckSize, ifs.cur);
		ckTotalSize -= 8;
		ckTotalSize -= chunk.ckSize;
	}
	if (ckTotalSize > 0) {
		std::cerr << "Unexpected end of input file." << std::endl;
		return RETURN_CODE_BAD_FILE;
	}
	hdr.cbSize = (unsigned)o.tellp() - sizeof(FileHeader);
	size_t extSize = 0;
	char buffer[CHAR_BUFFER_SIZE];
	while (!ifs.eof()) {
		ifs.read(buffer, sizeof(buffer));
		o.write(buffer, ifs.gcount());
		extSize += ifs.gcount();
	}
	if (extSize > 0) {
		COUT << "* Input file have a binary tail " << extSize << " bytes" << std::endl;
	}
	ifs.close();
	std::ios::fmtflags fmt(COUT.flags());
	if (calculateCRC) {
		hdr.flags |= FileFlags::CRC;
		std::ifstream res(inputFile, std::ofstream::binary);
		hdr.crc32 = getFileCrc32(res);
		res.close();
		COUT << "File CRC32: " << std::uppercase << std::hex << hdr.crc32 << std::endl;	
	}	
	o.seekp(0, o.beg);
	o.write((const char *)&hdr, sizeof(hdr));
	o.close();
	auto endTime = std::chrono::steady_clock::now();
	FLOAT ratio = (FLOAT)dataSize/compressedSize;
	if (compressedSize > 0) {
		COUT.unsetf(std::ios::floatfield);
		COUT.precision(2);
		COUT << "Compression ratio: " << ratio << std::endl;
		COUT.precision(4);
		COUT << "Bits per sample: " << comm.sampleSize / ratio << std::endl;
	}
	if (overflow_count > 0) {
		COUT << "Overflows: " << overflow_count << std::endl;
	}
	double cputime = (double)std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
	if (cputime > 0) {
		COUT << "Processing took " << cputime << " sec (" <<
			(comm.numSampleFrames / comm.sampleRate) / cputime << " x real-time)" << std::endl;
	}
	COUT.flags(fmt);
	return 0;
}

int decompress(const std::string &inputFile, const std::string &outputFile)
{
	std::ifstream ifs(inputFile, std::ifstream::binary);
	if (!ifs.good()) {
		std::cerr << "Can't open input file " << inputFile << std::endl;
		return  RETURN_CODE_IO_ERROR;
	}
		
	FileHeader hdr;
	ifs.read((char *)&hdr, sizeof(FileHeader));
	if (hdr.id != 'HPAK') {
		ifs.close();
		std::cerr << "Not " << APP_NAME << " input file." << std::endl;
		return RETURN_CODE_BAD_FILE;
	}
	if (hdr.version != APP_VERSION) {
		ifs.close();
		std::cerr << "Version " << hdr.version << "is not supported by " << APP_NAME << "." << std::endl;
		return RETURN_CODE_BAD_FILE;
	}

	COUT << "Decode file " << getfilename(inputFile) << std::endl;

	level = hdr.flags & FileFlags::MEM_MASK;
	xrnd.reset();

	int ret = RETURN_CODE_OK;
	if ((hdr.flags & FileFlags::AIFF) == FileFlags::AIFF) {
		ret = decompressAIFF(ifs, inputFile, outputFile, hdr);
	}
	else {
		ret = RETURN_CODE_BAD_FILE;
	}

	ifs.close();
	return ret;
}

int decompressAIFF(std::ifstream& ifs, const std::string &inputFile, std::string outputFile, FileHeader &hdr)
{
	if (outputFile == "") {
		size_t lastindex = inputFile.find_last_of(".");
		std::string rawname = inputFile.substr(0, lastindex);
		outputFile = rawname.append(".aif");
	}

	std::ofstream o(outputFile, std::ofstream::binary);
	if (!o.good()) {
		std::cerr << "Can't open output file " << outputFile << std::endl;
		return  -1;
	}

	auto startTime = std::chrono::steady_clock::now();

	Chunk rootChunk;
	rootChunk.chkID = 'FORM';
	rootChunk.ckSize = 0;
	chunkw(o, rootChunk);	
	ID formType = 'AIFF';
	formType = BINARY_SWAP32(formType);
	o.write((const char *)&formType, sizeof(formType));

	long ckTotalSize = hdr.cbSize;

	CommonChunk comm;
	comm.numChannels = 1;
	comm.sampleSize = 8;
	comm.numSampleFrames = 0;

	while (!ifs.eof() && ckTotalSize > 0) {
		Chunk chunk;
		chunkh(ifs, chunk);
		chunkw(o, chunk);
		if (chunk.chkID == 'COMM') {
			LOGCHUNK(chunk);
			comm.h = chunk;
			readComm(ifs, comm);
			chunkcp(ifs, o, chunk);
			std::ios::fmtflags fmt(COUT.flags());
			COUT << "Number Channels: " << comm.numChannels << std::endl;
			COUT << "Number of Sample Frames: " << comm.numSampleFrames << std::endl;
			COUT << "Resolution: " << comm.sampleSize << " bit" << std::endl;
			COUT.precision(4);
			COUT << "Frequency: " << comm.sampleRate / 1000.0 << " kHz" << std::endl;
			COUT << "Play time: " << (int)floor(comm.numSampleFrames / comm.sampleRate / 60) << " min " <<
				(int)floor(comm.numSampleFrames / comm.sampleRate) % 60 << " sec" << std::endl;
			COUT.flags(fmt);
		}
		else if (chunk.chkID == 'SSND') {
			Chunk newchunk = chunk;
			newchunk.ckSize = (int)ssndupack(ifs, o, comm.sampleSize, comm.numChannels, comm.numSampleFrames);
			LOGCHUNK(newchunk);
			chunkrw(o, newchunk);			
		}
		else {
			LOGCHUNK(chunk);
			chunkcp(ifs, o, chunk);
		}		
		ifs.seekg(chunk.ckSize, ifs.cur);
		ckTotalSize -= 8;
		ckTotalSize -= chunk.ckSize;
	}

	rootChunk.ckSize = (unsigned)o.tellp() - sizeof(Chunk) + sizeof(std::streampos);
	chunkrw(o, rootChunk);
	LOGCHUNK(rootChunk);
		
	size_t extSize = 0;
	char buffer[CHAR_BUFFER_SIZE];
	while (!ifs.eof()) {
		ifs.read(buffer, sizeof(buffer));
		o.write(buffer, ifs.gcount());
		extSize += ifs.gcount();
	}
	if (extSize > 0) {
		COUT << "* Packed file have a binary tail " << extSize << " bytes" << std::endl;
	}

	o.close();

	int ret = RETURN_CODE_OK;
	if (hdr.flags & FileFlags::CRC == FileFlags::CRC) {
		std::ifstream res(outputFile, std::ofstream::binary);
		U32 crc32 = getFileCrc32(res);
		res.close();
		std::ios::fmtflags fmt(COUT.flags());
		COUT << "File CRC32: " << std::uppercase << std::hex << crc32 << std::endl;
		COUT.flags(fmt);
		if (crc32 != hdr.crc32) {
			std::cerr << "CRC mismatch. Result file is corrupted." << std::endl;
			ret = RETURN_CODE_CRC_MISMATCH;
		}
	}	

	auto endTime = std::chrono::steady_clock::now();
	double cputime = (double)std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
	if (cputime > 0) {
		COUT.unsetf(std::ios::floatfield);
		COUT.precision(2);
		COUT << "Processing took " << cputime << " sec (" <<
			(comm.numSampleFrames / comm.sampleRate) / cputime << " x real-time)" << std::endl;
	}
	return ret;
}


