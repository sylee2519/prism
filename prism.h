#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <time.h>
#include <string>
#include <mutex>
#include <vector>
#include <math.h>
#include <stdlib.h>
#include <filesystem>
#include <algorithm>
#include <openssl/sha.h>
#include "uthash.h"
#include <mpi.h>
#include <unordered_map>
#include <map>
#include <utility>
#include <chrono>
#include <numeric>
#include <unordered_map>
#include <queue>
#include <regex>
#include <sys/stat.h>
#include <atomic>
#include <lustre/lustreapi.h>
#include <fcntl.h>
#include <boost/assign/list_of.hpp>
#include <cstring>

#define QUEUE_SIZE 100      // # of metadata(object info) within queue
#define BUFFER_SIZE 2097152 // 2M buffer
#define POOL_SIZE 100       // 100 buffers in buffer pool
#define OST_NUMBER 24
#define TASK_QUEUE_FULL 10
#define OST_QUEUE_FULL 100
#define MAX_FILE_PATH_LEN 320
#define MASTER 0
#define NUMMASTERS 11
#define TERMINATION_MSG "TERMINATION"
#define MB (uint64_t)1048576

using namespace std;

namespace prism
{

  enum chunk_size
  {
    SZ_4K,
    SZ_8K,
    SZ_16K,
    SZ_32K,
    SZ_128K,
    SZ_256K,
    SZ_512K,
    SZ_1MB,
    SZ_4MB,
    FULL_FILE
  };

  enum fingerprint_modes
  {
    FP_SHA1,
    FP_SHA256,
    FP_SHA512,
    FP_MD5
  };

  struct chunk_entry
  {
    uint64_t start_pos;
    unsigned char fingerprint[SHA_DIGEST_LENGTH];
  };

  class ChunkData
  {
  public:
    int mode;
    vector<chunk_entry *> chunks;
    ChunkData()
    {
      mode = 0;
    }
    void init()
    {
      for (vector<chunk_entry *>::iterator i = chunks.begin(); i != chunks.end(); ++i)
      {
        delete *i;
      }
    }
    ~ChunkData()
    {
      init();
    }
  };

  typedef struct
  {
    uint32_t mi;
    uint32_t av;
    uint32_t ma;
    uint32_t ns;
    uint32_t mask_s;
    uint32_t mask_l;
    size_t pos;
  } fcdc_ctx;

  typedef struct
  {
    size_t offset;
    size_t len;
  } chunk;

  typedef kvec_t(chunk) chunk_vec;

  typedef struct
  {
    int ost;        // ost index
    uint64_t start; // start offset
    uint64_t end;   // end offset
    uint64_t interval;
    uint64_t size;
    char file_path[MAX_FILE_PATH_LEN];
  } object_task;

  typedef struct
  {
    queue<object_task> taskQueue;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    // newly inserted code
    pthread_cond_t cond_full;

  } OST_queue;

  typedef struct
  {
    char data[BUFFER_SIZE];
    int filled;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint64_t size;
  } Buffer;

  class Dedupe
  {
    int chunk_mode;
    int chunk_block;
    int chunk_size;
    int fp_mode;
    int worldSize;
    string output_file;
    ChunkData cd;

    unordered_map<int, int> ost_map;
    vector<OST_queue> ost_q;
    Buffer bufferpool[POOL_SIZE];
    atomic<int> reader_idx = 0; // worker_idx=0;
    atomic<bool> shutdown_flag{false};
    atomic<bool> reader_done{false};
    int total_file = 0;
    int non_default_pfl = 0;
    uint64_t total_file_size = 0;
    int ostPerRank = 0;
    int ost_cnt = 0;
    int rank = 0;
    int obj_cnt = 0;
    int numWorkers = 1;
    int numMasters = 1;
    uint64_t task_cnt = 0;
    uint64_t task_cnt_per_rank = 0;
    vector<int> worker_counter;

    // variable for load balance functionality
    int load_balance = 1;
    uint64_t *size_per_rank;
    vector<object_task> tasks_per_ost[OST_NUMBER];
    uint64_t size_per_ost[OST_NUMBER] = {0};

    // test code
    uint64_t test_chunk_cnt = 0;

    // variable for configuration of Number of IO Threads (more than the number of OST)

    uint64_t num_tasks_per_ost[OST_NUMBER] = {0};

    string Dataset;
    enum dataset
    {
      mpas = 0,
      grims,
      qchem,
      roms,
      abaqus,
      lammps,
      qe,
      qmc,
      cesm,
      msc,
      grommacs,
      siesta,
      total
    };
    map<string, dataset> dataset_map = boost::assign::map_list_of("mpas", mpas)("grims", grims)("qchem", qchem)("roms", roms)("abaqus", abaqus)("lammps", lammps)("qe", qe)("qmc", qmc)("cesm", cesm)("msc", msc)("grommacs", grommacs)("siesta", siesta)("dataset", total);

    uint64_t StandardizedTaskSizePerProcess[12][4] =
        {
            {},                                             // 1mpas
            {},                                             // 2grims
            {3341 * MB, 0, 0, 0},                           // 3qchem
            {5515509760, 2143045504, 732649767, 857382650}, // 4roms
            {1780 * MB, 1780 * MB, 1780 * MB, 1780 * MB},
            //{5515509760,	2143045504,	732649767,	857382650}, // 4roms -> temp: expr to abaqus
            //{}, // 5abaqus
            {},                                             // 6lammps
            {2803391651, 1302645772, 849047750, 544757172}, // 7qe
            {},                                             // qmc
            {8573 * MB, 0, 0, 0},                           // cesm
            {4828968097, 2358521078, 0, 958400427},         // msc
            {},                                             // grommacs
            {14328 * MB, 0, 0, 0}                           // siesta
    };

    uint64_t StandardizedTaskSizePer24Process[10] =
        {
            917 * MB,  // mpas
            335 * MB,  // grims
            3342 * MB, // qchem <- nastran?
            // 53 * MB,	// nastran
            5260 * MB,  // roms
            10975 * MB, // 1780 * MB, // 2457*MB, 		//5070 * MB,	//10975 * MB,	// abaqus
            120 * MB,   // 351 * MB,	// lammps
            8796 * MB,  // qe
            82 * MB,    // qmc
            10 * MB,    // pytorch
            2000 * MB   // dataset 96 processses
    };

  public:
    fcdc_ctx fastcdc_init(uint32_t min_ch, uint32_t avg_ch, uint32_t max_ch);
    size_t fastcdc_update(fcdc_ctx *ctx, uint8_t *data, size_t len, int end, chunk_vec *cv);
    size_t fastcdc_stream(FILE *stream, uint32_t min_ch, uint32_t avg_ch, uint32_t max_ch, chunk_vec *cv);

    Dedupe(int chunk_mode_from, int ch_size, int fp_mode, string outfile, int numWorkers, int load_balance)
        : worker_counter(OST_NUMBER, 0)
    {
      chunk_mode = chunk_mode_from;
      chunk_size = ch_size;
      this->fp_mode = fp_mode;
      output_file = outfile;
      this->numWorkers = numWorkers;
      this->load_balance = load_balance;
    }
    void chunk_full_file(string file_name, ofstream &);
    void chunk_fixed_size(const string &buffer, uint64_t obj_size);
    void chunk_cdc(string file_name, ofstream &, chunk_vec *cv, uint64_t *);
    void chunk_cdc(string, ofstream &);
    int traverse_directory(string directory_path);
    void processFile(const string &file_name, int rank, int aggregator_rank, int worldSize);
    void gather_fingerprints(const std::vector<std::string> &local_fingerprints, std::vector<std::string> &all_fingerprints);
    string GetHexRepresentation(const unsigned char *Bytes, size_t Length);
    string readFile(const string &fileName);
    static void *commThreadStarter(void *arg);
    static void *readerThreadStarter(void *arg);
    static void *workerThreadStarter(void *arg);
    void *workerThread(int w_idx);
    void *readerThread();
    void *commThread();
    void object_tasks_decomposition(const char *Msg, int cnt);
    void enqueue(OST_queue *ost_q, object_task task);
    object_task dequeue(OST_queue *ost_q);
    void initializeq(int ostPerRank);

    // Multiple Master  code
    int parse_file_idx(string file_path);

    void test_load_balance_per_rank();

    // void layout_analysis(std::filesystem::directory_entry entry);
    void layout_analysis(filesystem::directory_entry entry, vector<vector<object_task *>> &task_queue);
    void layout_end_of_process(vector<vector<object_task *>> &task_queue, int master_idx);
    object_task *object_task_generate(const char *file_path, int ost, uint64_t start, uint64_t end, uint64_t interval, uint64_t size);

    // load balance distribution code

    static bool taskQueueCompare(const object_task *task1, const object_task *task2);
    void object_task_load_balance(vector<vector<object_task *>> &task_queue);

    void object_task_insert(object_task *task, vector<object_task *>);
    char *object_task_queue_clear(vector<object_task *> &task_queue, int *task_num);
    void object_task_serialization(object_task *task, char *buffer);
    object_task *object_task_deserialization(const char *);
    void object_task_buffer_free(char *);
    void Msg_Push(char *buffer, char *Msg, int idx);

    // Utility Function
    double calculateMean(vector<double> list);
    double calculateStddev(vector<double> list, double mean);
    void output_log(const char *log_file_name, double data1, double data2);
    void output_log(const char *log_file_name, const char *thread_type, int rank, double exec_time);
  };

  typedef struct
  {
    Dedupe *instance;
    int index;
  } ThreadArgs;

  class FStat
  {
    long long total_file_count;
    long long total_file_size;
    long long mean_file_size;

    vector<pair<long long, string>> fsize_table;

    unordered_map<string, long long> fe_table;

  public:
    FStat()
    {
      total_file_count = 0;
      total_file_size = 0;
      mean_file_size = 0;
    }

    // -f option 1
    void measure_file_sizes(string directory_path, ofstream &output_file);

    // -f option 2
    void measure_cumulative_fs(string directory_path, ofstream &output_file);

    // -f option 3
    void measure_file_extensions(string directory_path, ofstream &output_file);
  };
}
