#ifndef __CNM_HEAD

#define __CNM_HEAD
# include <bits/extc++.h>

// 这些头必须放在宏定义之前：下面有 #define ret return，
// 而 OpenSSL 头里有名为 ret 的形参，顺序反了会报
// "expected ',' or '...' before 'return'"。
# include <openssl/ssl.h>
# include <openssl/err.h>
# include <openssl/rand.h>
# include <openssl/evp.h>
# include <sys/socket.h>
# include <netdb.h>
# include <unistd.h>
# include <sys/time.h>

# define rep(i, a, b) for(int i = (a);i <= (b);i ++)
# define lop(i, a, b) for(int i = (a);i <  (b);i ++)
# define dwn(i, a, b) for(int i = (a);i >= (b);i --)
# define int64 long long
# define ___main signed main()
# define f64 double
# define f32 float
# define f128 long double
# define int128 __int128
# define i128 ((__int128)1)
# define int32 signed
# define un unsigned
# define int16 short
# define int8 char
# define ret return
# define inl inline
# define reg register
# define fn void
# define elif else if
# define pb push_back
# define pp pop_back
# define CASES() int T;cin >> T;while(T--)
# define FaILurEg(s) freopen(s".in","r",stdin);freopen(s".out","w",stdout)
#if defined(_MSC_VER)
#  define FORCE_INLINE __forceinline
#else
#  define FORCE_INLINE __attribute__((always_inline)) inline
#endif
namespace packin {
using std::cout;
using std::cin;
template<typename T>FORCE_INLINE fn read(T &x) {cin >> x;}
template<typename T, typename... Args>FORCE_INLINE void read(T &x, Args&... args) {read(x);read(args...);}
#if __cplusplus >= 202000L
template<typename T>fn read(std::span<T> X) {for(auto &x : X) read(x);}
template<typename T>fn print(std::span<const T> X, std::string empt = " "){for(auto x : X) print(x, empt);}
template<typename T>fn error(std::span<const T> X, std::string empt = " "){for(auto x : X) error(x, empt);}
#endif
inline void print_impl(const char* s) {while (*s) {cout << *s++;}}
template<typename T, typename... Args>inline void print_impl(const char* s, const T& x, const Args&... args) {while (*s) {if (s[0] == '{' && s[1] == '}') {cout << x;print_impl(s + 2, args...);return;}cout << *s++;}}
template<typename... Args>inline void print(const char* s, const Args&... args) {print_impl(s, args...);}
inline void error_impl(const char*s){while(*s)std::cerr<<*s++;}template<class T,class...A>inline void error_impl(const char*s,const T&x,const A&...a){while(*s){if(s[0]=='{'&&s[1]=='}'){std::cerr<<x;error_impl(s+2,a...);return;}std::cerr<<*s++;}}template<class...A>inline void error(const char*s,const A&...a){error_impl(s,a...);}
void IOS() {std::ios::sync_with_stdio(0);std::cin.tie(0);}}
template<typename T>FORCE_INLINE T min(T a, T b) {ret (a < b)?a:b;}
template<typename T>FORCE_INLINE T max(T a, T b) {ret (a > b)?a:b;}
using packin::read;
using packin::print;
using packin::error;
using std::pair;
using std::vector;using packin::IOS;
#define DEFp(a, A, b, B) typedef pair<a, b> p##A##B;
#define DEFM(a, A, b, B) typedef std::map<a, b> $m##A##B;
#define DEFMu(a, A, b, B) typedef std::unordered_map<a, b> $um##A##B;
#define DEFG(a, A, b, B) typedef __gnu_pbds::gp_hash_table<a, b> $g##A##B;
#define FOR_EACH_LEFT(m)  \
    m(int, i)             \
    m(long long, l)      \
    m(double, d)          \
    m(long double, ld)         
#define FOR_EACH_RIGHT(m, T1, S1) \
    m(T1, S1, int, i)             \
    m(T1, S1, long long, l)      \
    m(T1, S1, double, d)          \
    m(T1, S1, long double, ld)  

// 笛卡尔积展开
#define EXPAND_LEFT(T1, S1) FOR_EACH_RIGHT(DEFp, T1, S1)

// 一句触发
FOR_EACH_LEFT(EXPAND_LEFT)
#undef EXPAND_LEFT

#define EXPAND_LEFT(T1, S1) FOR_EACH_RIGHT(DEFM, T1, S1)
FOR_EACH_LEFT(EXPAND_LEFT)
#undef EXPAND_LEFT

#define EXPAND_LEFT(T1, S1) FOR_EACH_RIGHT(DEFMu, T1, S1)
FOR_EACH_LEFT(EXPAND_LEFT)
#undef EXPAND_LEFT

#define EXPAND_LEFT(T1, S1) FOR_EACH_RIGHT(DEFG, T1, S1)
FOR_EACH_LEFT(EXPAND_LEFT)
#undef EXPAND_LEFT

#define DEFX(TYPE, TY, X, x) typedef TYPE<X> TY##x;

#define MAKELIST(m, TYPE, TY) m(TYPE, TY, int, i)m(TYPE, TY, long long, l)m(TYPE, TY, double, d)m(TYPE, TY, long double, ld)  

MAKELIST(DEFX, vector, $v);MAKELIST(DEFX, std::basic_string, $bs);MAKELIST(DEFX, std::set, $S);
//w: word d Dword q qword
typedef short $w;typedef int $d;typedef int64 $q;typedef int128 $o;
typedef char $b;
typedef un char u$b;
typedef un short u$w;typedef un int u$d;typedef un int64 u$q;typedef un int128 u$o;
typedef float $fd;typedef double $fq;typedef long double $fqu;/*Quint* */typedef __float128 $fo;
#if __cplusplus>=202300
typedef _Float16 $fw;typedef __bf16 $fbw;
#endif
typedef std::string _Str;

#endif