# D-Drive — Distributed File Storage Across Multiple Google Drive Accounts

D-Drive is a C++ CLI application that aggregates free storage from multiple Google Drive accounts into one virtual drive, using parallel chunk-striping to achieve **4x–7x faster** uploads and downloads than a single account allows.

---

## Demo

```
$ ./filesplitter
Welcome to D-Drive CLI
Type 'help' to list available commands
>> add-account
  Opening browser for Google OAuth...
  Account user@gmail.com added!
>> upload /path/to/large-video.mp4
  [===========================>      ] 72% | 48.3 MB/s | ETA: 00:12
>> list
  --- Uploaded Files ---
  large-video.mp4 (8 chunks)
>> download large-video.mp4 ./output/large-video.mp4
  Download completed to: ./output/large-video.mp4
```

### Architecture Diagram

```
  Local File (e.g. 2 GB)
        │
        ▼
  ┌─────────────┐
  │  File Reader │  (Producer Thread)
  │  256 MB/chunk│
  └──────┬──────┘
         │  Thread-Safe Queue
   ┌─────┼─────┬─────┐
   ▼     ▼     ▼     ▼
[T1]  [T2]  [T3]  ...  (Consumer Threads, up to 16 concurrent)
  │     │     │
  ▼     ▼     ▼
Account1 Account2 Account3  (OAuth2-authenticated Google Drive accounts)
  │     │     │
  └─────┴─────┘
        │
        ▼
  metadata.json  (tracks file_id + account + part number for each chunk)
```

---

## Why I Built This

Google's free 15 GB Drive limit fills up fast — especially for videos, backups, and large datasets. Upgrading storage costs money, but most people have several Google accounts sitting idle. D-Drive solves this by transparently pooling those accounts into a single, fast, unified storage layer — no paid plan needed. It also served as a deep dive into concurrent C++ systems programming and real-world OAuth 2.0 integration.

---

## Key Technical Highlights

- **Producer–consumer pipeline with a thread-safe queue:** A dedicated producer thread reads the file and pushes 256 MB chunks into a lock-free-friendly queue; up to 16 async consumer threads pull from it and upload in parallel, maximising network saturation.
- **Semaphore-controlled concurrency:** A hand-rolled C++ semaphore (using `std::mutex` + `std::condition_variable`) caps simultaneous in-flight uploads to prevent rate-limiting and memory overload.
- **Embedded OAuth 2.0 server:** The `add-account` flow spins up a lightweight `cpp-httplib` HTTP server on `localhost:8080` solely to capture Google's redirect code — no manual copy-paste required.
- **Resumable uploads via Google Drive API:** Each chunk is sent through the Drive resumable upload protocol, making the transfer fault-tolerant against transient network errors.
- **JSON metadata for reliable reassembly:** Every uploaded chunk's Drive file ID, owning account, and part number are persisted to `metadata.json`, guaranteeing bit-perfect reconstruction regardless of upload order.

---

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C++17 |
| Build system | CMake 3.15+ |
| Dependency manager | vcpkg |
| HTTP client | [CPR (C++ Requests)](https://github.com/libcpr/cpr) |
| HTTP server (OAuth) | [cpp-httplib](https://github.com/yhirose/cpp-httplib) |
| JSON | [nlohmann/json](https://github.com/nlohmann/json) |
| Progress bars | [Indicators](https://github.com/p-ranav/indicators) |
| Cloud backend | Google Drive REST API v3 |
| Auth | OAuth 2.0 Authorization Code Flow |

---

## How to Run Locally

### 1. Google Cloud Setup (one-time)

1. Go to [Google Cloud Console](https://console.cloud.google.com/) and create a project.
2. Enable the **Google Drive API** under *APIs & Services → Enabled APIs*.
3. Go to *APIs & Services → OAuth consent screen*, choose **External**, and fill in the required fields.
4. Go to *APIs & Services → Credentials → + Create Credentials → OAuth client ID*, choose **Desktop app**.
5. Download the JSON file, rename it `credentials.json`, and place it at `data/credentials/credentials.json` inside the project root.

### 2. Prerequisites

- C++17 compiler (GCC 9+, Clang 10+, or MSVC 2019+)
- [CMake](https://cmake.org/download/) ≥ 3.15
- [vcpkg](https://github.com/microsoft/vcpkg)

### 3. Build

```bash
git clone https://github.com/deepgodhani/D-Drive.git
cd D-Drive

# Install dependencies
vcpkg install nlohmann-json cpr indicators cpp-httplib

# Configure (replace <vcpkg-root> with your vcpkg installation path)
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release
```

### 4. Run

```bash
./build/filesplitter          # Linux/macOS
.\build\Release\filesplitter  # Windows
```

### 5. First-time setup

```
>> add-account        # Authenticate one or more Google accounts
>> upload <file>      # Upload any file
>> list               # See stored files
>> download <name> <save_path>
>> delete <name>
>> help               # Full command reference
```

---

## Architecture Overview

D-Drive follows a **split → stripe → parallel-transfer → reassemble** pipeline:

1. **Splitting:** The source file is read sequentially by a producer thread and divided into 256 MB chunks pushed onto a thread-safe queue.
2. **Striping:** Consumer threads pop chunks from the queue and assign each to a different Google Drive account in round-robin order (chunk `i` → `accounts[i % n]`).
3. **Parallel upload:** Up to 16 consumer threads upload concurrently; a semaphore prevents exceeding this limit. Each chunk is sent using Drive's resumable upload protocol.
4. **Metadata persistence:** On success, each chunk's Drive file ID, account email, and part index are appended to `metadata.json`.
5. **Download & reassembly:** All chunks are fetched in parallel, written to a temp directory, then concatenated in part-number order into the final output file.
6. **Authentication:** Each account token is stored as `data/tokens/<email>.json` and automatically refreshed via the OAuth 2.0 token endpoint when expired.

---

## Known Limitations / What I'd Improve

- **Metadata is local only:** `metadata.json` is stored on disk; if lost, uploaded files cannot be recovered. A future version should sync metadata to one of the Drive accounts itself.
- **No encryption:** Chunks are stored in plaintext on Google Drive. Adding AES-256 encryption before upload would make the tool suitable for sensitive data.
- **No partial failure recovery:** If a chunk upload fails mid-way, the whole upload is marked failed. Retry logic with exponential back-off (stubbed in `DDConfig.h`) is not yet wired into `Shell.cpp`.
- **Round-robin only:** Chunk distribution doesn't account for remaining storage per account; adding a capacity-aware scheduler would prevent any single account from filling up.
- **Single-machine only:** There is no server component — all metadata and tokens live on the machine running the CLI. A thin REST layer would enable multi-device access.