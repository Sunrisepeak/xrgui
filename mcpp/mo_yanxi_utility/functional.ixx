// mo_yanxi.functional -- std::move_only_function where the library has it.
//
// Same shape and same reason as mo_yanxi.views: libc++ has not shipped
// std::move_only_function (P0288), and this project holds 24 of them across 9
// files -- every task queue, every event callback, the main loop's init hook.
// Measured on clang 22.1.8 / libc++:
//
//   error: no member named 'move_only_function' in namespace 'std'
//
// and the symptom is not that message. The failing member declaration takes the
// whole class with it, so thread_pool reports `use of undeclared identifier
// 'queue_'` three lines further down, about a member that is fine.
//
// Where the library has it, this is a using-declaration and nothing else runs.
// The fallback below is compiled only by a toolchain that would otherwise not
// build the project at all, and it should be deleted the day libc++ ships the
// real thing.
//
// WHAT THE FALLBACK IS NOT
//
// It is not std::move_only_function. It supports the two signature forms this
// repository uses -- `R(Args...)` and `R(Args...) const` -- and not the `&`,
// `&&` or `noexcept` qualified ones, which are absent here and would each be a
// further specialisation. It always heap-allocates: the standard type has a
// small-object buffer, and callables that would have fitted in it now cost an
// allocation. That is a real cost, paid only on libc++, and it is the reason
// this is a shim rather than a replacement.

module;

// <version> for the __cpp_lib_* feature-test macros below.
//
// They are PREPROCESSOR macros and `import std;` does not define them --
// a module exports no macros. Without this header every #if below is
// false on every compiler, so the fallback is chosen even where the
// library has the real thing, and the shim silently stops being a shim.
// That is exactly what happened: MSVC took the local enumerate and its
// forward+sized requirement rejected a std::stacktrace.
#include <version>

export module mo_yanxi.functional;

import std;

namespace mo_yanxi::detail{

// Type-erased storage. The deleter is a plain function pointer rather than a
// virtual destructor so the class stays movable with no allocation of its own
// beyond the target, and so an empty function costs one null pointer.
using erased_storage = std::unique_ptr<void, void (*)(void*) noexcept>;

inline void erased_noop(void*) noexcept{
}

template <typename Sig>
class move_only_function_impl;

template <typename R, typename... A>
class move_only_function_impl<R(A...)>{
	erased_storage obj_{nullptr, &erased_noop};
	R (*invoke_)(void*, A&&...) = nullptr;

public:
	using result_type = R;

	move_only_function_impl() noexcept = default;
	move_only_function_impl(std::nullptr_t) noexcept{
	}

	template <typename F, typename D = std::decay_t<F>>
		requires (!std::same_as<D, move_only_function_impl>) && std::invocable<D&, A...> &&
		         std::convertible_to<std::invoke_result_t<D&, A...>, R>
	move_only_function_impl(F&& f)
		: obj_{new D(std::forward<F>(f)),
		       +[](void* p) noexcept{ delete static_cast<D*>(p); }},
		  invoke_{+[](void* p, A&&... a) -> R{
			  return std::invoke(*static_cast<D*>(p), std::forward<A>(a)...);
		  }}{
	}

	// The moved-from object must be EMPTY, not merely have a null target: a
	// defaulted move leaves invoke_ pointing at the thunk with obj_ null, and
	// `operator bool` then answers true for a function that would dereference
	// nullptr on call.
	move_only_function_impl(move_only_function_impl&& o) noexcept
		: obj_{std::move(o.obj_)}, invoke_{std::exchange(o.invoke_, nullptr)}{
	}

	move_only_function_impl& operator=(move_only_function_impl&& o) noexcept{
		obj_    = std::move(o.obj_);
		invoke_ = std::exchange(o.invoke_, nullptr);
		return *this;
	}

	move_only_function_impl(const move_only_function_impl&)            = delete;
	move_only_function_impl& operator=(const move_only_function_impl&) = delete;

	move_only_function_impl& operator=(std::nullptr_t) noexcept{
		obj_.reset();
		invoke_ = nullptr;
		return *this;
	}

	R operator()(A... a){
		return invoke_(obj_.get(), std::forward<A>(a)...);
	}

	[[nodiscard]] explicit operator bool() const noexcept{ return invoke_ != nullptr; }

	friend bool operator==(const move_only_function_impl& f, std::nullptr_t) noexcept{
		return !static_cast<bool>(f);
	}

	void swap(move_only_function_impl& o) noexcept{
		obj_.swap(o.obj_);
		std::swap(invoke_, o.invoke_);
	}
};

// `R(Args...) const` -- the call operator is const and the target is invoked as
// const. vk.context's resize event map holds one of these.
template <typename R, typename... A>
class move_only_function_impl<R(A...) const>{
	erased_storage obj_{nullptr, &erased_noop};
	R (*invoke_)(void*, A&&...) = nullptr;

public:
	using result_type = R;

	move_only_function_impl() noexcept = default;
	move_only_function_impl(std::nullptr_t) noexcept{
	}

	template <typename F, typename D = std::decay_t<F>>
		requires (!std::same_as<D, move_only_function_impl>) && std::invocable<const D&, A...> &&
		         std::convertible_to<std::invoke_result_t<const D&, A...>, R>
	move_only_function_impl(F&& f)
		: obj_{new D(std::forward<F>(f)),
		       +[](void* p) noexcept{ delete static_cast<D*>(p); }},
		  invoke_{+[](void* p, A&&... a) -> R{
			  return std::invoke(*static_cast<const D*>(p), std::forward<A>(a)...);
		  }}{
	}

	move_only_function_impl(move_only_function_impl&& o) noexcept
		: obj_{std::move(o.obj_)}, invoke_{std::exchange(o.invoke_, nullptr)}{
	}

	move_only_function_impl& operator=(move_only_function_impl&& o) noexcept{
		obj_    = std::move(o.obj_);
		invoke_ = std::exchange(o.invoke_, nullptr);
		return *this;
	}

	move_only_function_impl(const move_only_function_impl&)            = delete;
	move_only_function_impl& operator=(const move_only_function_impl&) = delete;

	move_only_function_impl& operator=(std::nullptr_t) noexcept{
		obj_.reset();
		invoke_ = nullptr;
		return *this;
	}

	R operator()(A... a) const{
		return invoke_(obj_.get(), std::forward<A>(a)...);
	}

	[[nodiscard]] explicit operator bool() const noexcept{ return invoke_ != nullptr; }

	friend bool operator==(const move_only_function_impl& f, std::nullptr_t) noexcept{
		return !static_cast<bool>(f);
	}

	void swap(move_only_function_impl& o) noexcept{
		obj_.swap(o.obj_);
		std::swap(invoke_, o.invoke_);
	}
};

} // namespace mo_yanxi::detail

export namespace mo_yanxi{

#if defined(__cpp_lib_move_only_function)
using std::move_only_function;
#else
template <typename Sig>
using move_only_function = detail::move_only_function_impl<Sig>;
#endif

} // namespace mo_yanxi
