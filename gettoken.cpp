# include <bits/extc++.h>
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
typedef un short u$w;typedef un int u$d;typedef un int64 u$q;typedef un int128 u$o;
typedef float $fd;typedef double $fq;typedef long double $fqu;/*Quint* */typedef __float128 $fo;
#if __cplusplus>=202300
typedef _Float16 $fw;typedef __bf16 $fbw;
#endif
typedef std::string _Str;

template <typename T, typename Provider>
class arr_ptr {
public:
   using element_type    = T;
   using pointer         = const T*;
   using reference       = const T&;
   using difference_type = std::ptrdiff_t;
   using size_type       = std::size_t;

private:
   size_type idx_ = 0;                       // 唯一状态：下标

   static constexpr pointer base() noexcept {          // 数组首地址由 Provider 给出
      return Provider::get();
   }

public:
   constexpr arr_ptr() noexcept = default;
   constexpr explicit arr_ptr(size_type i) noexcept : idx_(i) {}

   // ---------- 指针式访问 ----------
   reference operator*()  const noexcept { return base()[idx_]; }
   pointer   operator->() const noexcept { return base() + idx_; }
   reference operator[](difference_type n) const noexcept {
      return base()[idx_ + n];
   }

   pointer   get()   const noexcept { return base() + idx_; }  // 转成原生指针
   size_type index() const noexcept { return idx_; }           // 取下标
   void  set_index(size_type i) noexcept { idx_ = i; }         // 设置下标

   // ---------- 算术 ----------
   arr_ptr& operator+=(difference_type n) noexcept { idx_ += n; return *this; }
   arr_ptr& operator-=(difference_type n) noexcept { idx_ -= n; return *this; }

   friend arr_ptr operator+(arr_ptr p, difference_type n) noexcept { p += n; return p; }
   friend arr_ptr operator+(difference_type n, arr_ptr p) noexcept { p += n; return p; }
   friend arr_ptr operator-(arr_ptr p, difference_type n) noexcept { p -= n; return p; }
   friend difference_type operator-(arr_ptr a, arr_ptr b) noexcept {
      return static_cast<difference_type>(a.idx_)
          - static_cast<difference_type>(b.idx_);
   }

   // ---------- 自增 / 自减 ----------
   arr_ptr& operator++()    noexcept { ++idx_; return *this; }
   arr_ptr  operator++(int) noexcept { auto t = *this; ++idx_; return t; }
   arr_ptr& operator--()    noexcept { --idx_; return *this; }
   arr_ptr  operator--(int) noexcept { auto t = *this; --idx_; return t; }

   // ---------- 比较 ----------
   friend bool operator==(arr_ptr a, arr_ptr b) noexcept { return a.idx_ == b.idx_; }
   friend bool operator!=(arr_ptr a, arr_ptr b) noexcept { return a.idx_ != b.idx_; }
   friend bool operator< (arr_ptr a, arr_ptr b) noexcept { return a.idx_ <  b.idx_; }
   friend bool operator<=(arr_ptr a, arr_ptr b) noexcept { return a.idx_ <= b.idx_; }
   friend bool operator> (arr_ptr a, arr_ptr b) noexcept { return a.idx_ >  b.idx_; }
   friend bool operator>=(arr_ptr a, arr_ptr b) noexcept { return a.idx_ >= b.idx_; }

   // ---------- 显式转换为原生指针 ----------
   explicit operator pointer() const noexcept { return get(); }
};
___main {
   FILE* log = fopen("tokens.out", "w");
   $d pref;print("prefix:");read(pref);
   rep(i, 1, 999) {
      FILE *fp;
      sbs:
      fp = popen(("curl --url \'https://pb.ika.zip/api/paint-token\' -H \'content-type: application/json\' --data-raw \'{\"uid\":"+ std::to_string(pref * 1000 + i) + "}\'").c_str(), "r");
      static char X[200];
      if(fp == NULL)
         goto sbs;
      $d ptr = 0;
      fgets(X, 200, fp);
      if(pclose(fp) != 0) goto sbs;
      ptr = 16;
      fputc('\"', log);
      while(X[ptr] != '\"')
         fputc(X[ptr ++], log);
      fputc('\"', log);
      fputc(',', log);
      fputc('\n', log);
      print("{}\n{}\n", i, X);
      std::cout.flush();
      fflush(stdout);
      fflush(log);
   }
   /*
   curl --url 'https://pb.ika.zip/api/paint-token' -H 'content-type: application/json' --data-raw '{"uid":1}'
{"paint_token":"00000000-0000-0000-0000-000000000000","uid":1}%      
   */
   ret 0;}