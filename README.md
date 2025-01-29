# BitTorrent Protocol Simulation using MPI

This project simulates the BitTorrent peer-to-peer file-sharing protocol using MPI (Message Passing Interface) and Pthreads. The implementation focuses on decentralized file distribution, swarm management, and efficient segment downloading across multiple clients.

## Features

- **Tracker**: Manages swarm information for files and coordinates client interactions.

- **Clients**: Act as peers or seeds depending on their file segments.

- **Multi-threading**: Uses Pthreads for concurrent download and upload operations.

- **Efficiency**: Balances segment downloads across peers to avoid overloading a single source.

- **Simulation**: Simulates file transfers using hash-based segments instead of actual files.

## Requirements

- **MPI**: OpenMPI or MPICH (tested with OpenMPI 4.1.2).

- **C++ Compiler**: Supports C++11 or higher (e.g., `g++` or `clang`).

- **Pthreads**: Standard threading library (included with most systems).

## Installation & Usage

### Compilation

Compile the project using the provided `Makefile`:

```bash

make build  # Generates the executable `BitTorrent`

```

### Input Files

Each client (process rank > 0) requires an input file named `in<R>.txt`, where `R` is the client's MPI rank, stored in the same folder as the executable.

**Example structure** (`in1.txt`):

```

2

file1 3

3fcfb9d1242fdcec64aee2bfe3526691

6dd6078d720fc86c885f7f87faf0d32a

b56195fb830b234e8e35acaabc399400

file2 2

0f2ab6f4eab22e6b061f99daa48dd74e

f70fee606c4e57add77b3773fed6622b

1

file3

```

### Running the Simulation

Start the simulation with MPI:

```bash

mpirun -np <N> ./BitTorrent

```

- `<N>` must be ≥ 3 (1 tracker + ≥2 clients).

- Ensure input files (`in1.txt`, `in2.txt`, etc.) are in the working directory.

### Output

After completing downloads, clients generate output files named `client<R>_<FILENAME>` (e.g., `client2_file1`), containing the ordered hash segments of the downloaded file.

---

## Architecture

### Components

1. **Tracker (Rank 0)**:

- Maintains swarm data (seeds/peers per file).

- Responds to client requests for swarm updates.

- Coordinates client initialization and shutdown.

2. **Clients (Rank > 0)**:

- **Download Thread**: Requests missing segments from peers/seeds and updates the tracker.

- **Upload Thread**: Responds to segment requests from other clients.

- Supports role transitions (leecher → peer → seed).

### Workflow

1. **Initialization**:

- Clients send owned file metadata (filenames, segment hashes) to the tracker.

- Tracker builds initial swarm lists and signals clients to start downloading.

2. **Download Phase**:

- Clients request swarm lists for desired files.

- Segments are downloaded in a round-robin fashion from available peers/seeds.

- Swarm lists are refreshed every 10 segments to account for new peers.

3. **Completion**:

- Clients become seeds for downloaded files.

- Tracker terminates all processes once all downloads are complete.

---

## Efficiency

- **Balanced Downloads**: Clients prioritize less busy peers/seeds to avoid bottlenecks.

- **Dynamic Swarms**: Regular updates ensure fresh peer lists for optimal resource usage.

- **Parallelism**: Multi-threaded design maximizes upload/download throughput.

---

## Contributing

Contributions to improve this project or extend its capabilities are welcome. Please submit pull requests with detailed descriptions of changes or improvements.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE.txt) file for details.
