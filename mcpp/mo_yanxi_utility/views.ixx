// mo_yanxi.views -- the C++23 range adaptors this project uses, wherever the
// standard library it is built against actually has them.
//
// WHY THIS EXISTS
//
// libc++ has not shipped four of the C++23 adaptors. Measured, clang 22.1.8
// against its own libc++ and against GCC 16.1's libstdc++ (the latter guards
// them off under a non-GNU compiler, so pointing clang at a newer libstdc++
// does not help):
//
//   adaptor              libc++   libstdc++ 16
//   views::enumerate     absent   present
//   views::stride        absent   present
//   views::slide         absent   present
//   views::chunk         absent   present
//
// and the ones the project also uses -- zip, zip_transform, chunk_by, adjacent,
// join, take, transform -- are present in both, so they are not here.
//
// THE SHAPE
//
// Each name below is `using std::views::X;` where the library has X, and a
// local definition only where it does not. So MSVC and libstdc++ keep the
// standard implementations with their full iterator categories and their
// optimisations, and the code written here is compiled only by a toolchain that
// would otherwise reject the file outright. Call sites spell
// `mo_yanxi::views::X` and read the same on every platform.
//
// The feature-test macros are the ones the standard specifies for each adaptor;
// this file does not test for a compiler or a library by name.
//
// WHAT THE FALLBACKS PROMISE
//
// Not conformance -- these are not standard-quality adaptors, and they are not
// meant to become any. They cover the uses in this repository:
//
//   * a forward, sized base range (every call site has one)
//   * range-for and structured bindings
//   * composing with each other and with the standard adaptors through `|`
//   * constexpr (color::string_to_rgba runs this chain at compile time)
//   * being returned from a function (pipeline_manager::get_state_range)
//
// A base that is merely input, or unsized, does not compile -- deliberately,
// with the static_assert below saying so, because silently degrading is how a
// shim stops being a shim. Every fallback here should be deleted the day libc++
// ships the real thing; the #if is what makes that a one-line change.

module;

export module mo_yanxi.views;

import std;

namespace mo_yanxi::views::detail{

// The requirement every fallback shares. Stated once so the diagnostic is the
// same sentence wherever it is hit.
template <typename R>
concept shim_base = std::ranges::forward_range<R> && std::ranges::sized_range<R>;

inline constexpr std::string_view shim_base_message =
	"mo_yanxi::views fallback requires a forward, sized range. The standard "
	"adaptor accepts more; this one is only present because libc++ has not "
	"shipped it. Widen the shim, or reshape the call.";

// ── enumerate ──────────────────────────────────────────────────────────────
//
// Element is (index, base reference). The reference is the base's own, so
// writing through `auto&& [i, x]` writes to the underlying range exactly as the
// standard adaptor does.
template <std::ranges::view V>
	requires shim_base<V>
class enumerate_view : public std::ranges::view_interface<enumerate_view<V>>{
	V base_{};

public:
	using difference_type = std::ranges::range_difference_t<V>;

	class iterator{
		std::ranges::iterator_t<V> cur_{};
		difference_type             idx_{};

	public:
		using iterator_concept = std::conditional_t<
			std::ranges::random_access_range<V>, std::random_access_iterator_tag,
			std::conditional_t<std::ranges::bidirectional_range<V>,
			                   std::bidirectional_iterator_tag,
			                   std::forward_iterator_tag>>;
		using iterator_category = std::input_iterator_tag;
		using difference_type   = std::ranges::range_difference_t<V>;
		using value_type        = std::tuple<difference_type, std::ranges::range_value_t<V>>;

		iterator() = default;
		constexpr iterator(std::ranges::iterator_t<V> cur, difference_type idx)
			: cur_{std::move(cur)}, idx_{idx}{
		}

		constexpr auto operator*() const{
			return std::tuple<difference_type, std::ranges::range_reference_t<V>>{idx_, *cur_};
		}

		constexpr iterator& operator++(){ ++cur_; ++idx_; return *this; }
		constexpr iterator  operator++(int){ auto t = *this; ++*this; return t; }

		constexpr iterator& operator--()
			requires std::ranges::bidirectional_range<V>
		{ --cur_; --idx_; return *this; }
		constexpr iterator operator--(int)
			requires std::ranges::bidirectional_range<V>
		{ auto t = *this; --*this; return t; }

		constexpr iterator& operator+=(difference_type n)
			requires std::ranges::random_access_range<V>
		{ cur_ += n; idx_ += n; return *this; }
		constexpr iterator& operator-=(difference_type n)
			requires std::ranges::random_access_range<V>
		{ cur_ -= n; idx_ -= n; return *this; }

		friend constexpr iterator operator+(iterator i, difference_type n)
			requires std::ranges::random_access_range<V>
		{ return i += n; }
		friend constexpr iterator operator+(difference_type n, iterator i)
			requires std::ranges::random_access_range<V>
		{ return i += n; }
		friend constexpr iterator operator-(iterator i, difference_type n)
			requires std::ranges::random_access_range<V>
		{ return i -= n; }
		friend constexpr difference_type operator-(const iterator& a, const iterator& b)
			requires std::ranges::random_access_range<V>
		{ return a.idx_ - b.idx_; }

		constexpr auto operator[](difference_type n) const
			requires std::ranges::random_access_range<V>
		{ return *(*this + n); }

		friend constexpr bool operator==(const iterator& a, const iterator& b){
			return a.cur_ == b.cur_;
		}
		friend constexpr auto operator<=>(const iterator& a, const iterator& b)
			requires std::ranges::random_access_range<V>
		{ return a.idx_ <=> b.idx_; }

		// Compared against the base's sentinel when the base is not common.
		friend constexpr bool operator==(const iterator& a,
		                                 const std::ranges::sentinel_t<V>& s){
			return a.cur_ == s;
		}
	};

	// Conditional: a view over a non-default-constructible base (ref_view,
	// adjacent_view over one) is itself not default-constructible, and an
	// unconditional default ctor makes the whole class ill-formed for it.
	enumerate_view() requires std::default_initializable<V> = default;
	constexpr explicit enumerate_view(V base) : base_{std::move(base)}{
	}

	constexpr auto begin(){ return iterator{std::ranges::begin(base_), 0}; }

	constexpr auto end(){
		if constexpr(std::ranges::common_range<V>){
			return iterator{std::ranges::end(base_),
			                static_cast<difference_type>(std::ranges::size(base_))};
		} else {
			return std::ranges::end(base_);
		}
	}

	// Two overloads, not one const. `shim_base` guarantees V is sized; it says
	// nothing about `const V`, and a view over adjacent_view/ref_view is not
	// const-sized -- ranges::size on the const base then has no viable
	// overload and the whole class fails to instantiate.
	constexpr auto size() requires std::ranges::sized_range<V>
	{ return std::ranges::size(base_); }
	constexpr auto size() const requires std::ranges::sized_range<const V>
	{ return std::ranges::size(base_); }
};

template <typename R>
enumerate_view(R&&) -> enumerate_view<std::views::all_t<R>>;

struct enumerate_fn : std::ranges::range_adaptor_closure<enumerate_fn>{
	template <std::ranges::viewable_range R>
	constexpr auto operator()(R&& r) const{
		static_assert(shim_base<std::views::all_t<R>>, "enumerate: see shim_base_message");
		return enumerate_view{std::forward<R>(r)};
	}
};

// ── stride ─────────────────────────────────────────────────────────────────
//
// Every `n`-th element. The last step is clamped at the end rather than run
// past it, which is what makes `stride` safe on a range whose size is not a
// multiple of `n`.
template <std::ranges::view V>
	requires shim_base<V>
class stride_view : public std::ranges::view_interface<stride_view<V>>{
	V                                 base_{};
	std::ranges::range_difference_t<V> step_{1};

public:
	using difference_type = std::ranges::range_difference_t<V>;

	class iterator{
		std::ranges::iterator_t<V> cur_{};
		std::ranges::iterator_t<V> last_{};
		difference_type            step_{1};
		difference_type            idx_{};

	public:
		using iterator_concept = std::conditional_t<
			std::ranges::random_access_range<V>, std::random_access_iterator_tag,
			std::forward_iterator_tag>;
		using iterator_category = std::input_iterator_tag;
		using difference_type   = std::ranges::range_difference_t<V>;
		using value_type        = std::ranges::range_value_t<V>;

		iterator() = default;
		constexpr iterator(std::ranges::iterator_t<V> cur, std::ranges::iterator_t<V> last,
		                   difference_type step, difference_type idx)
			: cur_{std::move(cur)}, last_{std::move(last)}, step_{step}, idx_{idx}{
		}

		constexpr decltype(auto) operator*() const{ return *cur_; }

		constexpr iterator& operator++(){
			// Bounded: ranges::next stops at `last_` instead of stepping past it.
			cur_ = std::ranges::next(cur_, step_, last_);
			++idx_;
			return *this;
		}
		constexpr iterator operator++(int){ auto t = *this; ++*this; return t; }

		friend constexpr bool operator==(const iterator& a, const iterator& b){
			return a.cur_ == b.cur_;
		}
		friend constexpr bool operator==(const iterator& a,
		                                 const std::ranges::sentinel_t<V>& s){
			return a.cur_ == s;
		}
	};

	// Conditional: a view over a non-default-constructible base (ref_view,
	// adjacent_view over one) is itself not default-constructible, and an
	// unconditional default ctor makes the whole class ill-formed for it.
	stride_view() requires std::default_initializable<V> = default;
	constexpr stride_view(V base, std::ranges::range_difference_t<V> step)
		: base_{std::move(base)}, step_{step}{
	}

	constexpr auto begin(){
		return iterator{std::ranges::begin(base_), std::ranges::end(base_), step_, 0};
	}
	constexpr auto end(){
		if constexpr(std::ranges::common_range<V>){
			return iterator{std::ranges::end(base_), std::ranges::end(base_), step_,
			                static_cast<difference_type>(size())};
		} else {
			return std::ranges::end(base_);
		}
	}

	// Two overloads, not one const. `shim_base` guarantees V is sized; it says
	// nothing about `const V`, and a view over adjacent_view/ref_view is not
	// const-sized -- ranges::size on the const base then has no viable
	// overload and the whole class fails to instantiate.
	constexpr auto size() requires std::ranges::sized_range<V>{
		const auto n = static_cast<difference_type>(std::ranges::size(base_));
		return static_cast<std::size_t>((n + step_ - 1) / step_);
	}
	constexpr auto size() const requires std::ranges::sized_range<const V>{
		const auto n = static_cast<difference_type>(std::ranges::size(base_));
		return static_cast<std::size_t>((n + step_ - 1) / step_);
	}
};

template <typename R, typename D>
stride_view(R&&, D) -> stride_view<std::views::all_t<R>>;

struct stride_fn{
	template <std::ranges::viewable_range R>
	constexpr auto operator()(R&& r, std::ranges::range_difference_t<R> n) const{
		static_assert(shim_base<std::views::all_t<R>>, "stride: see shim_base_message");
		return stride_view{std::forward<R>(r), n};
	}

	template <typename D>
	struct closure : std::ranges::range_adaptor_closure<closure<D>>{
		D n;
		template <std::ranges::viewable_range R>
		constexpr auto operator()(R&& r) const{
			return stride_fn{}(std::forward<R>(r),
			                   static_cast<std::ranges::range_difference_t<R>>(n));
		}
	};

	template <typename D>
		requires std::integral<D>
	constexpr auto operator()(D n) const{ return closure<D>{{}, n}; }
};

// ── slide ──────────────────────────────────────────────────────────────────
//
// Every window of `w` consecutive elements. The element type is
// std::ranges::subrange, which is what gives the window `.data()` and `.size()`
// on a contiguous base -- color::string_to_rgba relies on both.
template <std::ranges::view V>
	requires shim_base<V>
class slide_view : public std::ranges::view_interface<slide_view<V>>{
	V                                  base_{};
	std::ranges::range_difference_t<V> width_{1};

public:
	using difference_type = std::ranges::range_difference_t<V>;

	class iterator{
		std::ranges::iterator_t<V> cur_{};
		difference_type            width_{1};

	public:
		using iterator_concept = std::conditional_t<
			std::ranges::random_access_range<V>, std::random_access_iterator_tag,
			std::forward_iterator_tag>;
		using iterator_category = std::input_iterator_tag;
		using difference_type   = std::ranges::range_difference_t<V>;
		using value_type = std::ranges::subrange<std::ranges::iterator_t<V>>;

		iterator() = default;
		constexpr iterator(std::ranges::iterator_t<V> cur, difference_type width)
			: cur_{std::move(cur)}, width_{width}{
		}

		constexpr auto operator*() const{
			return std::ranges::subrange{cur_, std::ranges::next(cur_, width_)};
		}

		constexpr iterator& operator++(){ ++cur_; return *this; }
		constexpr iterator  operator++(int){ auto t = *this; ++*this; return t; }

		friend constexpr bool operator==(const iterator& a, const iterator& b){
			return a.cur_ == b.cur_;
		}
	};

	// Conditional: a view over a non-default-constructible base (ref_view,
	// adjacent_view over one) is itself not default-constructible, and an
	// unconditional default ctor makes the whole class ill-formed for it.
	slide_view() requires std::default_initializable<V> = default;
	constexpr slide_view(V base, std::ranges::range_difference_t<V> width)
		: base_{std::move(base)}, width_{width}{
	}

	constexpr auto begin(){ return iterator{std::ranges::begin(base_), width_}; }

	// One past the LAST FULL window: a partial tail is not a window. When the
	// base is shorter than the window there are none at all, and begin() must
	// equal end() rather than run backwards.
	constexpr auto end(){
		const auto n = static_cast<difference_type>(std::ranges::size(base_));
		const auto count = n >= width_ ? n - width_ + 1 : difference_type{0};
		return iterator{std::ranges::next(std::ranges::begin(base_), count), width_};
	}

	// Two overloads, not one const. `shim_base` guarantees V is sized; it says
	// nothing about `const V`, and a view over adjacent_view/ref_view is not
	// const-sized -- ranges::size on the const base then has no viable
	// overload and the whole class fails to instantiate.
	constexpr auto size() requires std::ranges::sized_range<V>{
		const auto n = static_cast<difference_type>(std::ranges::size(base_));
		return static_cast<std::size_t>(n >= width_ ? n - width_ + 1 : 0);
	}
	constexpr auto size() const requires std::ranges::sized_range<const V>{
		const auto n = static_cast<difference_type>(std::ranges::size(base_));
		return static_cast<std::size_t>(n >= width_ ? n - width_ + 1 : 0);
	}
};

template <typename R, typename D>
slide_view(R&&, D) -> slide_view<std::views::all_t<R>>;

struct slide_fn{
	template <std::ranges::viewable_range R>
	constexpr auto operator()(R&& r, std::ranges::range_difference_t<R> w) const{
		static_assert(shim_base<std::views::all_t<R>>, "slide: see shim_base_message");
		return slide_view{std::forward<R>(r), w};
	}

	template <typename D>
	struct closure : std::ranges::range_adaptor_closure<closure<D>>{
		D w;
		template <std::ranges::viewable_range R>
		constexpr auto operator()(R&& r) const{
			return slide_fn{}(std::forward<R>(r),
			                  static_cast<std::ranges::range_difference_t<R>>(w));
		}
	};

	template <typename D>
		requires std::integral<D>
	constexpr auto operator()(D w) const{ return closure<D>{{}, w}; }
};

// ── chunk ──────────────────────────────────────────────────────────────────
//
// Consecutive, non-overlapping groups of `n`. Unlike slide, the final group may
// be short: that is the difference between the two, and dropping the tail here
// would silently lose elements.
template <std::ranges::view V>
	requires shim_base<V>
class chunk_view : public std::ranges::view_interface<chunk_view<V>>{
	V                                  base_{};
	std::ranges::range_difference_t<V> n_{1};

public:
	using difference_type = std::ranges::range_difference_t<V>;

	class iterator{
		std::ranges::iterator_t<V> cur_{};
		std::ranges::iterator_t<V> last_{};
		difference_type            n_{1};

	public:
		using iterator_concept  = std::forward_iterator_tag;
		using iterator_category = std::input_iterator_tag;
		using difference_type   = std::ranges::range_difference_t<V>;
		using value_type = std::ranges::subrange<std::ranges::iterator_t<V>>;

		iterator() = default;
		constexpr iterator(std::ranges::iterator_t<V> cur, std::ranges::iterator_t<V> last,
		                   difference_type n)
			: cur_{std::move(cur)}, last_{std::move(last)}, n_{n}{
		}

		constexpr auto operator*() const{
			return std::ranges::subrange{cur_, std::ranges::next(cur_, n_, last_)};
		}

		constexpr iterator& operator++(){
			cur_ = std::ranges::next(cur_, n_, last_);
			return *this;
		}
		constexpr iterator operator++(int){ auto t = *this; ++*this; return t; }

		friend constexpr bool operator==(const iterator& a, const iterator& b){
			return a.cur_ == b.cur_;
		}
	};

	// Conditional: a view over a non-default-constructible base (ref_view,
	// adjacent_view over one) is itself not default-constructible, and an
	// unconditional default ctor makes the whole class ill-formed for it.
	chunk_view() requires std::default_initializable<V> = default;
	constexpr chunk_view(V base, std::ranges::range_difference_t<V> n)
		: base_{std::move(base)}, n_{n}{
	}

	constexpr auto begin(){
		return iterator{std::ranges::begin(base_), std::ranges::end(base_), n_};
	}
	constexpr auto end(){
		return iterator{std::ranges::end(base_), std::ranges::end(base_), n_};
	}

	// Two overloads, not one const. `shim_base` guarantees V is sized; it says
	// nothing about `const V`, and a view over adjacent_view/ref_view is not
	// const-sized -- ranges::size on the const base then has no viable
	// overload and the whole class fails to instantiate.
	constexpr auto size() requires std::ranges::sized_range<V>{
		const auto n = static_cast<difference_type>(std::ranges::size(base_));
		return static_cast<std::size_t>((n + n_ - 1) / n_);
	}
	constexpr auto size() const requires std::ranges::sized_range<const V>{
		const auto n = static_cast<difference_type>(std::ranges::size(base_));
		return static_cast<std::size_t>((n + n_ - 1) / n_);
	}
};

template <typename R, typename D>
chunk_view(R&&, D) -> chunk_view<std::views::all_t<R>>;

struct chunk_fn{
	template <std::ranges::viewable_range R>
	constexpr auto operator()(R&& r, std::ranges::range_difference_t<R> n) const{
		static_assert(shim_base<std::views::all_t<R>>, "chunk: see shim_base_message");
		return chunk_view{std::forward<R>(r), n};
	}

	template <typename D>
	struct closure : std::ranges::range_adaptor_closure<closure<D>>{
		D n;
		template <std::ranges::viewable_range R>
		constexpr auto operator()(R&& r) const{
			return chunk_fn{}(std::forward<R>(r),
			                  static_cast<std::ranges::range_difference_t<R>>(n));
		}
	};

	template <typename D>
		requires std::integral<D>
	constexpr auto operator()(D n) const{ return closure<D>{{}, n}; }
};

} // namespace mo_yanxi::views::detail

export namespace mo_yanxi::ranges{

// P2278's range_const_reference_t, which libc++ has not shipped either. Unlike
// const_iterator it needs no machinery -- the standard defines it as exactly
// this common_reference_t -- so the fallback is the definition rather than an
// approximation of it.
#if defined(__cpp_lib_ranges_as_const)
using std::ranges::range_const_reference_t;
#else
template <std::ranges::range R>
using range_const_reference_t =
	std::common_reference_t<const std::ranges::range_value_t<R>&&,
	                        std::ranges::range_reference_t<R>>;
#endif

} // namespace mo_yanxi::ranges

export namespace mo_yanxi::views{

#if defined(__cpp_lib_ranges_enumerate)
using std::views::enumerate;
#else
inline constexpr detail::enumerate_fn enumerate{};
#endif

#if defined(__cpp_lib_ranges_stride)
using std::views::stride;
#else
inline constexpr detail::stride_fn stride{};
#endif

#if defined(__cpp_lib_ranges_slide)
using std::views::slide;
#else
inline constexpr detail::slide_fn slide{};
#endif

#if defined(__cpp_lib_ranges_chunk)
using std::views::chunk;
#else
inline constexpr detail::chunk_fn chunk{};
#endif

} // namespace mo_yanxi::views
