#include "prism.h"

int main(int argc, char **argv)
{

    uint64_t chunk_size = 0;
    string directory_path;
    string output_file;
    int chunk_mode = 0;
    int c;
    int fstat_flag = 0;
    int rank;
    int numWorkers = 1;
    int load_balance = 1;
    int numProc;
    while ((c = getopt(argc, argv, "f:s:m:i:o:t:l:n:")) != -1)
    {
        switch (c)
        {
        case 'f':
            fstat_flag = atoi(optarg);
            break;
        case 's':
            chunk_size = atoi(optarg);
            break;
        case 'm':
            chunk_mode = atoi(optarg);
            break;
        case 'i':
            directory_path = optarg;
            break;
        case 'o':
            output_file = optarg;
            break;
        case 't':
            numWorkers = atoi(optarg);
            break;
        case 'l':
            load_balance = atoi(optarg);
            break;
        case 'n':
            numProc = atoi(optarg);
            break;
        default:
            break;
        }
    }
    auto start = chrono::high_resolution_clock::now();
    if (fstat_flag == 1)
    {
        prism::FStat *fstat = new prism::FStat();
        ofstream fstat_file;

        fstat_file.open(output_file, ios::out);
        if (!fstat_file)
        {
            cout << "Out file error" << endl;
            exit(0);
        }

        fstat->measure_file_sizes(directory_path, fstat_file);

        cout << endl;
        cout << "File Statistics session end . . ." << endl;

        return 0;
    }

    else if (fstat_flag == 2)
    {
        prism::FStat *fstat = new prism::FStat();
        ofstream fstat_file;

        fstat_file.open(output_file, ios::out);
        if (!fstat_file)
        {
            cout << "Out file error" << endl;
            exit(0);
        }

        fstat->measure_cumulative_fs(directory_path, fstat_file);

        cout << endl;
        cout << "* Cumulative distribution session end . . ." << endl;

        return 0;
    }

    else if (fstat_flag == 3)
    {
        prism::FStat *fe = new prism::FStat();
        ofstream fe_file;

        fe_file.open(output_file, ios::out);
        if (!fe_file)
        {
            cout << "Out file error" << endl;
            exit(0);
        }

        fe->measure_file_extensions(directory_path, fe_file);

        cout << endl;
        cout << "** Measuring file extensions session end . . ." << endl;

        return 0;
    }

    else
    {
        prism::Dedupe *dedup = new prism::Dedupe(chunk_mode, chunk_size, 0, output_file, numWorkers, load_balance);

        rank = dedup->traverse_directory(directory_path);
    }
    auto end = chrono::high_resolution_clock::now();

    chrono::duration<double> duration = end - start;

    if (rank == 0)
    {
        size_t lastSlash = directory_path.rfind('/');
        string Dataset = directory_path.substr(lastSlash + 1);

        if (Dataset.empty())
            Dataset = "total";

        printf("dataset: %d\n", Dataset);

        string result = "standardized_load_result.eval";
        ofstream ofs(result, ios::out | ios::app);

        if (!ofs)
        {
            cerr << "Error opening output file\n";
            exit(0);
        }
        ofs << Dataset << '\t' << numProc << '\t' << "object" << '\t' << ((load_balance) ? "lb" : "none") << '\t' << duration.count() << endl;
        cout << "\tExecution time: " << duration.count() << " seconds" << endl;
    }
    return 0;
}

