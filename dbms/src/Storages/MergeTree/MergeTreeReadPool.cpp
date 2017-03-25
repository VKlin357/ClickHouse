#include <DB/Storages/MergeTree/RangesInDataPart.h>
#include <DB/Storages/MergeTree/MergeTreeReadPool.h>
#include <ext/range.hpp>


namespace ProfileEvents
{
	extern const Event SlowRead;
	extern const Event ReadBackoff;
}

namespace DB
{


MergeTreeReadPool::MergeTreeReadPool(
	const std::size_t threads, const std::size_t sum_marks, const std::size_t min_marks_for_concurrent_read,
	RangesInDataParts parts, MergeTreeData & data, const ExpressionActionsPtr & prewhere_actions,
	const String & prewhere_column_name, const bool check_columns, const Names & column_names,
	const BackoffSettings & backoff_settings,
	const bool do_not_steal_tasks)
	: backoff_settings{backoff_settings}, backoff_state{threads},
	data{data}, column_names{column_names}, do_not_steal_tasks{do_not_steal_tasks}
{
	const auto per_part_sum_marks = fillPerPartInfo(parts, prewhere_actions, prewhere_column_name, check_columns);
	fillPerThreadInfo(threads, sum_marks, per_part_sum_marks, parts, min_marks_for_concurrent_read);
}


MergeTreeReadTaskPtr MergeTreeReadPool::getTask(const std::size_t min_marks_to_read, const std::size_t thread)
{
	const std::lock_guard<std::mutex> lock{mutex};

	/// If number of threads was lowered due to backoff, then will assign work only for maximum 'backoff_state.current_threads' threads.
	if (thread >= backoff_state.current_threads)
		return nullptr;

	if (remaining_thread_tasks.empty())
		return nullptr;

	const auto tasks_remaining_for_this_thread = !threads_tasks[thread].sum_marks_in_parts.empty();
	if (!tasks_remaining_for_this_thread && do_not_steal_tasks)
		return nullptr;

	const auto thread_idx = tasks_remaining_for_this_thread ? thread : *std::begin(remaining_thread_tasks);
	auto & thread_tasks = threads_tasks[thread_idx];

	auto & thread_task = thread_tasks.parts_and_ranges.back();
	const auto part_idx = thread_task.part_idx;

	auto & part = parts[part_idx];
	auto & marks_in_part = thread_tasks.sum_marks_in_parts.back();

	/// Get whole part to read if it is small enough.
	auto need_marks = std::min(marks_in_part, min_marks_to_read);

	/// Do not leave too little rows in part for next time.
	if (marks_in_part > need_marks &&
		marks_in_part - need_marks < min_marks_to_read)
		need_marks = marks_in_part;

	MarkRanges ranges_to_get_from_part;

	/// Get whole part to read if it is small enough.
	if (marks_in_part <= need_marks)
	{
		const auto marks_to_get_from_range = marks_in_part;

		/** Ranges are in right-to-left order, because 'reverse' was done in MergeTreeDataSelectExecutor
			*	 and that order is supported in 'fillPerThreadInfo'.
			*/
		ranges_to_get_from_part = thread_task.ranges;

		marks_in_part -= marks_to_get_from_range;

		thread_tasks.parts_and_ranges.pop_back();
		thread_tasks.sum_marks_in_parts.pop_back();

		if (thread_tasks.sum_marks_in_parts.empty())
			remaining_thread_tasks.erase(thread_idx);
	}
	else
	{
		/// Loop through part ranges.
		while (need_marks > 0 && !thread_task.ranges.empty())
		{
			auto & range = thread_task.ranges.back();

			const std::size_t marks_in_range = range.end - range.begin;
			const std::size_t marks_to_get_from_range = std::min(marks_in_range, need_marks);

			ranges_to_get_from_part.emplace_back(range.begin, range.begin + marks_to_get_from_range);
			range.begin += marks_to_get_from_range;
			if (range.begin == range.end)
			{
				std::swap(range, thread_task.ranges.back());
				thread_task.ranges.pop_back();
			}

			marks_in_part -= marks_to_get_from_range;
			need_marks -= marks_to_get_from_range;
		}

		/** Change order to right-to-left, for MergeTreeThreadBlockInputStream to get ranges with .pop_back()
			*  (order was changed to left-to-right due to .pop_back() above).
			*/
		std::reverse(std::begin(ranges_to_get_from_part), std::end(ranges_to_get_from_part));
	}

	return std::make_unique<MergeTreeReadTask>(
		part.data_part, ranges_to_get_from_part, part.part_index_in_query, column_names,
		per_part_column_name_set[part_idx], per_part_columns[part_idx], per_part_pre_columns[part_idx],
		per_part_remove_prewhere_column[part_idx], per_part_should_reorder[part_idx]);
}


void MergeTreeReadPool::profileFeedback(const ReadBufferFromFileBase::ProfileInfo info)
{
	if (backoff_settings.min_read_latency_ms == 0 || do_not_steal_tasks)
		return;

	if (info.nanoseconds < backoff_settings.min_read_latency_ms * 1000000)
		return;

	std::lock_guard<std::mutex> lock(mutex);

	if (backoff_state.current_threads <= 1)
		return;

	size_t throughput = info.bytes_read * 1000000000 / info.nanoseconds;

	if (throughput >= backoff_settings.max_throughput)
		return;

	if (backoff_state.time_since_prev_event.elapsed() < backoff_settings.min_interval_between_events_ms * 1000000)
		return;

	backoff_state.time_since_prev_event.restart();
	++backoff_state.num_events;

	ProfileEvents::increment(ProfileEvents::SlowRead);
	LOG_DEBUG(log, std::fixed << std::setprecision(3)
		<< "Slow read, event №" << backoff_state.num_events
		<< ": read " << info.bytes_read << " bytes in " << info.nanoseconds / 1000000000.0 << " sec., "
		<< info.bytes_read * 1000.0 / info.nanoseconds << " MB/s.");

	if (backoff_state.num_events < backoff_settings.min_events)
		return;

	backoff_state.num_events = 0;
	--backoff_state.current_threads;

	ProfileEvents::increment(ProfileEvents::ReadBackoff);
	LOG_DEBUG(log, "Will lower number of threads to " << backoff_state.current_threads);
}


std::vector<std::size_t> MergeTreeReadPool::fillPerPartInfo(
	RangesInDataParts & parts, const ExpressionActionsPtr & prewhere_actions, const String & prewhere_column_name,
	const bool check_columns)
{
	std::vector<std::size_t> per_part_sum_marks;

	for (const auto i : ext::range(0, parts.size()))
	{
		auto & part = parts[i];

		/// Read marks for every data part.
		size_t sum_marks = 0;
		/// Ranges are in right-to-left order, due to 'reverse' in MergeTreeDataSelectExecutor.
		for (const auto & range : part.ranges)
			sum_marks += range.end - range.begin;

		per_part_sum_marks.push_back(sum_marks);

		per_part_columns_lock.push_back(std::make_unique<Poco::ScopedReadRWLock>(
			part.data_part->columns_lock));

		/// inject column names required for DEFAULT evaluation in current part
		auto required_column_names = column_names;

		const auto injected_columns = injectRequiredColumns(part.data_part, required_column_names);
		auto should_reoder = !injected_columns.empty();

		Names required_pre_column_names;

		if (prewhere_actions)
		{
			/// collect columns required for PREWHERE evaluation
			required_pre_column_names = prewhere_actions->getRequiredColumns();

			/// there must be at least one column required for PREWHERE
			if (required_pre_column_names.empty())
				required_pre_column_names.push_back(required_column_names[0]);

			/// PREWHERE columns may require some additional columns for DEFAULT evaluation
			const auto injected_pre_columns = injectRequiredColumns(part.data_part, required_pre_column_names);
			if (!injected_pre_columns.empty())
				should_reoder = true;

			/// will be used to distinguish between PREWHERE and WHERE columns when applying filter
			const NameSet pre_name_set{
				std::begin(required_pre_column_names), std::end(required_pre_column_names)
			};
			/** If expression in PREWHERE is not table column, then no need to return column with it to caller
				*	(because storage is expected only to read table columns).
				*/
			per_part_remove_prewhere_column.push_back(0 == pre_name_set.count(prewhere_column_name));

			Names post_column_names;
			for (const auto & name : required_column_names)
				if (!pre_name_set.count(name))
					post_column_names.push_back(name);

			required_column_names = post_column_names;
		}
		else
			per_part_remove_prewhere_column.push_back(false);

		per_part_column_name_set.emplace_back(std::begin(required_column_names), std::end(required_column_names));

		if (check_columns)
		{
			/** Under part->columns_lock check that all requested columns in part are of same type that in table.
				*	This could be violated during ALTER MODIFY.
				*/
			if (!required_pre_column_names.empty())
				data.check(part.data_part->columns, required_pre_column_names);
			if (!required_column_names.empty())
				data.check(part.data_part->columns, required_column_names);

			per_part_pre_columns.push_back(data.getColumnsList().addTypes(required_pre_column_names));
			per_part_columns.push_back(data.getColumnsList().addTypes(required_column_names));
		}
		else
		{
			per_part_pre_columns.push_back(part.data_part->columns.addTypes(required_pre_column_names));
			per_part_columns.push_back(part.data_part->columns.addTypes(required_column_names));
		}

		per_part_should_reorder.push_back(should_reoder);

		this->parts.push_back({ part.data_part, part.part_index_in_query });
	}

	return per_part_sum_marks;
}


void MergeTreeReadPool::fillPerThreadInfo(
	const std::size_t threads, const std::size_t sum_marks, std::vector<std::size_t> per_part_sum_marks,
	RangesInDataParts & parts, const std::size_t min_marks_for_concurrent_read)
{
	threads_tasks.resize(threads);

	const size_t min_marks_per_thread = (sum_marks - 1) / threads + 1;

	for (std::size_t i = 0; i < threads && !parts.empty(); ++i)
	{
		auto need_marks = min_marks_per_thread;

		while (need_marks > 0 && !parts.empty())
		{
			const auto part_idx = parts.size() - 1;
			RangesInDataPart & part = parts.back();
			size_t & marks_in_part = per_part_sum_marks.back();

			/// Do not get too few rows from part.
			if (marks_in_part >= min_marks_for_concurrent_read &&
				need_marks < min_marks_for_concurrent_read)
				need_marks = min_marks_for_concurrent_read;

			/// Do not leave too few rows in part for next time.
			if (marks_in_part > need_marks &&
				marks_in_part - need_marks < min_marks_for_concurrent_read)
				need_marks = marks_in_part;

			MarkRanges ranges_to_get_from_part;
			size_t marks_in_ranges = need_marks;

			/// Get whole part to read if it is small enough.
			if (marks_in_part <= need_marks)
			{
				/// Leave ranges in right-to-left order for convenience to use .pop_back() in .getTask()
				ranges_to_get_from_part = part.ranges;
				marks_in_ranges = marks_in_part;

				need_marks -= marks_in_part;
				parts.pop_back();
				per_part_sum_marks.pop_back();
			}
			else
			{
				/// Loop through part ranges.
				while (need_marks > 0)
				{
					if (part.ranges.empty())
						throw Exception("Unexpected end of ranges while spreading marks among threads", ErrorCodes::LOGICAL_ERROR);

					MarkRange & range = part.ranges.back();

					const size_t marks_in_range = range.end - range.begin;
					const size_t marks_to_get_from_range = std::min(marks_in_range, need_marks);

					ranges_to_get_from_part.emplace_back(range.begin, range.begin + marks_to_get_from_range);
					range.begin += marks_to_get_from_range;
					marks_in_part -= marks_to_get_from_range;
					need_marks -= marks_to_get_from_range;
					if (range.begin == range.end)
						part.ranges.pop_back();
				}

				/** Change order to right-to-left, for getTask() to get ranges with .pop_back()
					*  (order was changed to left-to-right due to .pop_back() above).
					*/
				std::reverse(std::begin(ranges_to_get_from_part), std::end(ranges_to_get_from_part));
			}

			threads_tasks[i].parts_and_ranges.push_back({ part_idx, ranges_to_get_from_part });
			threads_tasks[i].sum_marks_in_parts.push_back(marks_in_ranges);
			if (marks_in_ranges != 0)
				remaining_thread_tasks.insert(i);
		}
	}
}


NameSet MergeTreeReadPool::injectRequiredColumns(const MergeTreeData::DataPartPtr & part, Names & columns) const
{
	NameSet required_columns{std::begin(columns), std::end(columns)};
	NameSet injected_columns;

	auto all_column_files_missing = true;

	for (size_t i = 0; i < columns.size(); ++i)
	{
		const auto & column_name = columns[i];

		/// column has files and hence does not require evaluation
		if (part->hasColumnFiles(column_name))
		{
			all_column_files_missing = false;
			continue;
		}

		const auto default_it = data.column_defaults.find(column_name);
		/// columns has no explicit default expression
		if (default_it == std::end(data.column_defaults))
			continue;

		/// collect identifiers required for evaluation
		IdentifierNameSet identifiers;
		default_it->second.expression->collectIdentifierNames(identifiers);

		for (const auto & identifier : identifiers)
		{
			if (data.hasColumn(identifier))
			{
				/// ensure each column is added only once
				if (required_columns.count(identifier) == 0)
				{
					columns.emplace_back(identifier);
					required_columns.emplace(identifier);
					injected_columns.emplace(identifier);
				}
			}
		}
	}

	/** Add a column of the minimum size.
		* Used in case when no column is needed or files are missing, but at least you need to know number of rows.
		* Adds to the columns.
		*/
	if (all_column_files_missing)
	{
		const auto minimum_size_column_name = part->getColumnNameWithMinumumCompressedSize();
		columns.push_back(minimum_size_column_name);
		/// correctly report added column
		injected_columns.insert(columns.back());
	}

	return injected_columns;
}


}
