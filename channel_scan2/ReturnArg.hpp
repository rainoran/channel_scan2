
#pragma once

#ifndef LIBISDB_RETURN_ARG_H
#define LIBISDB_RETURN_ARG_H


namespace ARIBDecoder
{

	template<typename T> class ReturnArg
	{
	public:
		ReturnArg(T *ptr) noexcept : m_ptr(ptr) {}

		ReturnArg & operator = (const T &rhs)
		{
			if (m_ptr != nullptr)
				*m_ptr = rhs;
			return *this;
		}

		ReturnArg & operator = (T &&rhs)
		{
			if (m_ptr != nullptr)
				*m_ptr = std::move(rhs);
			return *this;
		}

		explicit operator bool() const noexcept { return m_ptr != nullptr; }

		T & operator * () { return *m_ptr; }
		const T & operator * () const { return *m_ptr; }
		T * operator -> () noexcept { return m_ptr; }
		const T * operator -> () const noexcept { return m_ptr; }

	protected:
		ReturnArg() noexcept : m_ptr(nullptr) {}

		T *m_ptr;
	};

	template<typename T> class OptionalReturnArg
		: public ReturnArg<T>
	{
	public:
		OptionalReturnArg() = default;
		OptionalReturnArg(std::nullopt_t) noexcept {}
		using ReturnArg<T>::ReturnArg;
		using ReturnArg<T>::operator =;
	};

} // namespace LibISDB


#endif // ifndef LIBISDB_RETURN_ARG_H
