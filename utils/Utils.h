/*
Copyright (c) 2015-2019 Alternative Games Ltd / Turo Lamminen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


#ifndef UTILS_H
#define UTILS_H


#include <cinttypes>

#include <memory>
#include <string>
#include <vector>


#ifdef _MSC_VER

#define fileno _fileno
#define UNREACHABLE() abort()
#define PRINTF(x, y)
#define UNUSED

#else   // _MSC_VER

#define UNREACHABLE() __builtin_unreachable()
#define PRINTF(x, y) __attribute__((format(printf, x, y)))
#define UNUSED        __attribute__((unused))

#endif  // _MSC_VER


#if defined(__GNUC__) && defined(_WIN32)

#undef  PRIu64
#define PRIu64 "I64u"

#ifndef PRIx64
#define PRIx64 "llx"
#endif  // PRIx64

#endif  // defined(__GNUC__) && defined(_WIN32)


#ifdef _MSC_VER
#define __PRETTY_FUNCTION__  __FUNCTION__
#endif

#define STUBBED(str) \
	{ \
		static bool seen = false; \
		if (!seen) { \
			printf("STUBBED: %s in %s at %s:%d\n", str, __PRETTY_FUNCTION__, __FILE__,  __LINE__); \
			seen = true; \
		} \
	}


// should be ifdeffed out on compilers which already have it (eg. VS2013)
// http://isocpp.org/files/papers/N3656.txt
#ifndef _MSC_VER
namespace std {

    template<class T> struct _Unique_if {
        typedef unique_ptr<T> _Single_object;
    };

    template<class T> struct _Unique_if<T[]> {
        typedef unique_ptr<T[]> _Unknown_bound;
    };

    template<class T, size_t N> struct _Unique_if<T[N]> {
        typedef void _Known_bound;
    };

    template<class T, class... Args>
        typename _Unique_if<T>::_Single_object
        make_unique(Args&&... args) {
            return unique_ptr<T>(new T(std::forward<Args>(args)...));
        }

    template<class T>
        typename _Unique_if<T>::_Unknown_bound
        make_unique(size_t n) {
            typedef typename remove_extent<T>::type U;
            return unique_ptr<T>(new U[n]());
        }

    template<class T, class... Args>
        typename _Unique_if<T>::_Known_bound
        make_unique(Args&&...) = delete;


}  // namespace std
#endif


#define LOG(msg, ...) logWrite(msg, ##__VA_ARGS__)

void logInit();
void logWrite(const char* message, ...) PRINTF(1, 2);
void logShutdown();
void logFlush();

std::vector<char> readTextFile(std::string filename);
std::vector<char> readFile(std::string filename);
void writeFile(const std::string &filename, const void *contents, size_t size);
bool fileExists(const std::string &filename);
int64_t getFileTimestamp(const std::string &filename);

// From https://graphics.stanford.edu/~seander/bithacks.html#DetermineIfPowerOf2
static inline bool isPow2(unsigned int value) {
	return (value & (value - 1)) == 0;
}


// https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
static inline uint32_t nextPow2(unsigned int v) {
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;

	return v;
}


static inline uint64_t gcd(uint64_t a, uint64_t b) {
	uint64_t c;
	while (a != 0) {
		c = a;
		a = b % a;
		b = c;
	}
	return b;
}


#endif  // UTILS_H
