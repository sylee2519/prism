#include "prism.h"

extern int errno;

using namespace std;

namespace prism
{

	void Dedupe::layout_analysis(filesystem::directory_entry entry, vector<vector<object_task *>> &task_queue)
	{

		chrono::high_resolution_clock::time_point start_time, end_time;
		static int traverse_cnt = 0;
		if (traverse_cnt == 0)
		{
			traverse_cnt++;
			cout << "Hello world!\n";
			start_time = chrono::high_resolution_clock::now();
		}
		int rc[5];
		uint64_t count = 0, size, start, end, idx, offset, interval, file_size;
		struct stat64 sb;
		object_task *task;
		int worldSize = this->worldSize;
		int worker_number = worldSize - NUMMASTERS;
		int obj_idx = 0;

		// Get layouts of the file
		struct llapi_layout *layout;

		layout = llapi_layout_get_by_path(entry.path().c_str(), 0);
		if (layout == NULL)
		{
			cout << "errno:" << errno;
			return;
		}

		// Get size of the file.
		stat64(entry.path().c_str(), &sb);
		file_size = sb.st_size;
		total_file_size += file_size;

		// Get first layout of the file
		rc[0] = llapi_layout_comp_use(layout, 1);
		if (rc[0])
		{
			cout << "error: layout component iteration failed\n";
			return;
			// continue;
		}
		while (1)
		{

			// Get stripe count, stripe size, and range(in terms of the size) of the layout.
			rc[0] = llapi_layout_stripe_count_get(layout, &count);
			rc[1] = llapi_layout_stripe_size_get(layout, &size);
			rc[2] = llapi_layout_comp_extent_get(layout, &start, &end);
			if (rc[0] || rc[1] || rc[2])
			{
				cout << "error: cannot get stripe information\n";
				continue;
			}

			interval = count * size;
			end = (end < file_size) ? end : file_size;
			// For each stripe, Get OST index and create Task that contains the information about what to read that will be sent to each Reader Process
			for (int i = 0; i < count; i++)
			{

				// Get OST index
				rc[0] = llapi_layout_ost_index_get(layout, i, &idx);
				// If OST information cannot be taken, it means that we get all the OST idx that stores the object of the file and we can break out of the loop.

				if (rc[0])
				{
					goto here_exit;
				}
				// Generate the task and push it into corresponding task_queue (We allocate queue as much as the number of the OST).

				task = object_task_generate(entry.path().c_str(), idx, start + i * size, end, interval, size);
				this->task_cnt++;

				int task_ost = task->ost;

				task_queue[task_ost].push_back(task);

				// load balancing code
				uint64_t object_task_size = (end - start) / count;
				this->size_per_ost[task_ost] += object_task_size;

				int dest_rank, num_binded_worker;

				if (OST_NUMBER >= worker_number)
					dest_rank = task_ost % worker_number + 1;
				else // Otherwise, more than two worker process is binded to one OST, so tasks should be passed in round robin manner.
				{
					int remainder = task->ost < (worker_number % OST_NUMBER) ? 1 : 0;
					num_binded_worker = worker_number / OST_NUMBER + remainder;
					dest_rank = (num_tasks_per_ost[task_ost] % num_binded_worker) * OST_NUMBER + task_ost;
					if (dest_rank == 0)
						dest_rank = worker_number;
				}

				// If task queue is full, then we send the tasks in the queue to corresponding Reader Process.

				if (!load_balance && task_queue[task_ost].size() == TASK_QUEUE_FULL)
				{

					char *Msg = object_task_queue_clear(task_queue[task_ost], NULL);
					int dest_rank, num_binded_worker;

					// IF the number of OST exceeds the number of worker process, then Each OST is binded only to one worker process.
					if (OST_NUMBER >= worker_number)
						dest_rank = task_ost % worker_number + 1;
					else // Otherwise, more than two worker process is binded to one OST, so tasks should be passed in round robin manner.
					{
						int remainder = task_ost < (worker_number % OST_NUMBER) ? 1 : 0;
						num_binded_worker = worker_number / OST_NUMBER + remainder;
						dest_rank = (num_tasks_per_ost[task_ost] % num_binded_worker) * OST_NUMBER + task_ost;
						if (dest_rank == 0)
							dest_rank = worker_number;
					}
					num_tasks_per_ost[task_ost] += 1;

					int rc = MPI_Send(Msg, sizeof(object_task) * TASK_QUEUE_FULL, MPI_CHAR, dest_rank, TASK_QUEUE_FULL, MPI_COMM_WORLD);
					if (rc != MPI_SUCCESS)
						cout << "MPI Send Failed\n";
					delete[] Msg;
				}
			}
			rc[0] = llapi_layout_comp_use(layout, 3);
			if (rc[0] == 0)
				continue;
			if (rc[0] < 0)
				cout << "error: layout component iteration failed\n";
			break;
		}
	here_exit:
		return;
	}

	void Dedupe::layout_end_of_process(vector<vector<object_task *>> &task_queue, int master_idx)
	{
		int task_num;
		int worldSize = this->worldSize;
		char termination_task[20];
		strcpy(termination_task, TERMINATION_MSG);

		int worker_number;
		for (int ost = 0; ost < OST_NUMBER; ost++)
		{
			char *Msg = object_task_queue_clear(task_queue[ost], &task_num);
			if (Msg == nullptr)
			{
				cout << "Msg is NULL\n";
				continue;
			}

			int dest_rank, num_binded_worker;
			worker_number = worldSize - NUMMASTERS;

			if (OST_NUMBER >= worker_number)
				dest_rank = ost % worker_number + 1;
			else // Otherwise, more than two worker process is binded to one OST, so tasks should be passed in round robin manner.
			{
				int remainder = ost < (worker_number % OST_NUMBER) ? 1 : 0;
				num_binded_worker = worker_number / OST_NUMBER + remainder;
				dest_rank = (num_tasks_per_ost[ost] % num_binded_worker) * OST_NUMBER + ost;

				if (dest_rank == 0)
					dest_rank = worker_number;
			}

			int rc = MPI_Send(Msg, sizeof(object_task) * task_num, MPI_CHAR, dest_rank, task_num, MPI_COMM_WORLD);
			if (rc != MPI_SUCCESS)
				cout << "MPI Send Failed\n";
			object_task_buffer_free(Msg);
		}

		if (master_idx == MASTER)
		{
			for (int i = 1; i <= worker_number; i++)
			{
				MPI_Send(termination_task, sizeof(TERMINATION_MSG), MPI_CHAR, i, 0, MPI_COMM_WORLD);
			}
		}
	}

	void Dedupe::Msg_Push(char *buffer, char *Msg, int idx)
	{
		memcpy(Msg + idx * sizeof(object_task), buffer, sizeof(object_task));
	}

	bool Dedupe::taskQueueCompare(const object_task *task1, const object_task *task2)
	{
		uint64_t task_size1 = (task1->end - task1->start) / (task1->interval / task1->size);
		uint64_t task_size2 = (task2->end - task2->start) / (task2->interval / task2->size);

		return task_size1 > task_size2;
	}

	char *Dedupe::object_task_queue_clear(vector<object_task *> &task_queue, int *task_num)
	{

		char *Msg = new char[sizeof(object_task) * TASK_QUEUE_FULL];
		if (Msg == nullptr)
		{
			cout << "Memory allocation error\n";
			return nullptr;
		}

		if (task_num != nullptr)
		{

			if (task_queue.size() < TASK_QUEUE_FULL)
				*task_num = task_queue.size();
			else
				*task_num = TASK_QUEUE_FULL;
		}
		char *buffer = new char[sizeof(object_task)];
		if (buffer == nullptr)
		{
			cout << "Memory allocation error\n";
			return nullptr;
		}

		int idx = 0;
		while (!task_queue.empty())
		{
			object_task *task = task_queue.back();
			task_queue.pop_back();

			object_task_serialization(task, buffer);
			Msg_Push(buffer, Msg, idx);
			idx++;

			if (idx == TASK_QUEUE_FULL)
				break;
		}
		delete[] buffer;

		return Msg;
	}

	void Dedupe::object_task_buffer_free(char *buffer)
	{
		delete[] buffer;
	}

	// Serialize struct OBJECT_TASK into string so that it can be sent by MPI_SEND command.
	// Each element is delimited by ,(comma) and ENDOF struct is marked by '\n'
	void Dedupe::object_task_serialization(object_task *task, char *buffer)
	{
		int filepath_len = strlen(task->file_path);

		int *p = (int *)buffer;
		*p = task->ost;
		p++;

		uint64_t *q = (uint64_t *)p;
		*q = task->start;
		q++;
		*q = task->end;
		q++;
		*q = task->interval;
		q++;
		*q = task->size;
		q++;

		char *r = (char *)q;
		for (int i = 0; i < filepath_len; i++)
		{
			*r = task->file_path[i];
			r++;
		}
		*r = 0;
	}

	object_task *Dedupe::object_task_deserialization(const char *buffer)
	{
		object_task *task = new object_task;

		int *p = (int *)buffer;
		task->ost = *p;
		p++;

		uint64_t *q = (uint64_t *)p;
		task->start = *q;
		q++;
		task->end = *q;
		q++;
		task->interval = *q;
		q++;
		task->size = *q;
		q++;

		memcpy(task->file_path, q, MAX_FILE_PATH_LEN);
		return task;
	}

	void Dedupe::object_task_insert(object_task *task, vector<object_task *> queue)
	{
		queue.push_back(task);
		return;
	}

	object_task *Dedupe::object_task_generate(const char *file_path, int ost, uint64_t start, uint64_t end, uint64_t interval, uint64_t size)
	{
		// task should be deleted after completing the task on Reader process.
		object_task *task = new object_task;

		memcpy(task->file_path, file_path, strlen(file_path) + 1);

		task->ost = ost;
		task->start = start;
		task->end = end;
		task->interval = interval;
		task->size = size;

		return task;
	}

};
