#pragma once

#include <common/DateLUTImpl.h>
#include <common/singleton.h>
#include <DB/Core/Defines.h>

#include <unordered_map>
#include <atomic>
#include <mutex>
#include <memory>


/// This class provides lazy initialization and lookup of singleton DateLUTImpl objects for a given timezone.
class DateLUT : public Singleton<DateLUT>
{
	friend class Singleton<DateLUT>;

public:
	DateLUT(const DateLUT &) = delete;
	DateLUT & operator=(const DateLUT &) = delete;

	/// Return singleton DateLUTImpl instance for the default time zone.
	static ALWAYS_INLINE const DateLUTImpl & instance()
	{
		const auto & date_lut = Singleton<DateLUT>::instance();
		return *date_lut.default_impl.load(std::memory_order_acquire);
	}

	/// Return singleton DateLUTImpl instance for a given time zone.
	static ALWAYS_INLINE const DateLUTImpl & instance(const std::string & time_zone)
	{
		const auto & date_lut = Singleton<DateLUT>::instance();
		if (time_zone.empty())
			return *date_lut.default_impl.load(std::memory_order_acquire);

		return date_lut.getImplementation(time_zone);
	}

	static void setDefaultTimezone(const std::string & time_zone)
	{
		auto & date_lut = Singleton<DateLUT>::instance();
		const auto & impl = date_lut.getImplementation(time_zone);
		date_lut.default_impl.store(&impl, std::memory_order_release);
	}

protected:
	DateLUT();

private:
	const DateLUTImpl & getImplementation(const std::string & time_zone) const;

	using DateLUTImplPtr = std::unique_ptr<DateLUTImpl>;

	/// Time zone name -> implementation.
	mutable std::unordered_map<std::string, DateLUTImplPtr> impls;
	mutable std::mutex mutex;

	std::atomic<const DateLUTImpl *> default_impl;
};
