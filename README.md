# D-Drive: A Distributed File Storage System over Google Drive

D-Drive is a high-performance C++ command-line application that transforms multiple Google Drive accounts into a single, aggregated, and distributed file storage system. By intelligently splitting large files into smaller chunks and striping them across different accounts, it parallelizes transfers to dramatically increase upload and download speeds.

This project was built from the ground up to explore advanced concepts in system design, concurrency, and secure API integration in C++.

---

## Key Features

* **Distributed File Storage:** Aggregates the free storage of multiple Google Drive accounts into one massive virtual drive.
* **High-Speed Transfers:** Utilizes multi-threading to upload and download file chunks concurrently, achieving significant speedups (e.g., **4x-7x faster**) compared to a single-stream transfer.
* **Seamless Multi-Account Management:** A secure, built-in OAuth 2.0 flow allows users to easily add and manage multiple Google Drive accounts without ever leaving the command line.
* **Data Integrity:** A robust JSON-based metadata system tracks every chunk, its location, and its order, ensuring that files can be reliably reassembled without corruption.
* **Efficient Concurrency:** Employs a modern C++ semaphore-controlled thread pool to manage network operations, preventing system overload while maximizing throughput.
* **Interactive Shell:** Provides a user-friendly CLI to manage accounts, upload files, download files, and list stored content.

---

## Technical Deep Dive

### System Architecture
![image](https://github.com/deepgodhani/D-Drive/assets/112933366/a7f1a300-84c4-42c2-809f-d3097b69519c)


D-Drive operates on a simple yet powerful principle: **divide and conquer**.

1.  **File Splitting:** When a user initiates an upload, the file is split into fixed-size chunks (e.g., 50MB).
2.  **Chunk Distribution:** The application cycles through the list of authenticated Google Drive accounts, assigning each chunk to a different account.
3.  **Parallel Upload:** A pool of worker threads is used to upload these chunks concurrently. A semaphore limits the number of active uploads to prevent rate-limiting and manage resources efficiently.
4.  **Metadata Tracking:** As each chunk is successfully uploaded, its Google Drive File ID, the account it belongs to, and its part number are recorded in a local `metadata.json` file.
5.  **Reconstruction:** During a download, the application reads the metadata, fetches all chunks in parallel from their respective accounts, and reassembles them in the correct order to reconstruct the original file.

### Secure Authentication

To avoid storing user credentials directly, D-Drive implements the **OAuth 2.0 Authorization Code Flow**.

1.  When a user runs `add-account`, the application generates a unique authorization URL.
2.  The user's default web browser is automatically opened to this URL for them to grant permission.
3.  Upon granting permission, Google redirects the browser to a local `http://localhost:8080` address.
4.  The application runs a lightweight, embedded HTTP server that listens for this redirect, captures the authorization code, and immediately shuts down.
5.  This code is exchanged for a refresh token and an access token, which are then stored securely for future API calls.

---

## Prerequisites

* A C++17 compliant compiler (e.g., GCC, Clang, MSVC)
* [CMake](https://cmake.org/download/) (version 3.10 or higher)
* [vcpkg](https://github.com/microsoft/vcpkg) for dependency management

## Dependencies

This project relies on the following excellent open-source libraries, which will be automatically managed by `vcpkg`:

* [**nlohmann/json**](https://github.com/nlohmann/json): For all JSON parsing and serialization.
* [**CPR (C++ Requests)**](https://github.com/libcpr/cpr): A modern, elegant HTTP library for making API calls to Google Drive.
* [**cpp-httplib**](https://github.com/yhirose/cpp-httplib): A single-header/file C++ HTTP/HTTPS server and client library, used for the OAuth 2.0 redirect flow.
* [**Indicators**](https://github.com/p-ranav/indicators): For creating beautiful, interactive progress bars.

---

## Setup and Installation

### 1. Google Cloud Platform Setup

Before you can build, you need to get your own Google Drive API credentials.

1.  Go to the [Google Cloud Console](https://console.cloud.google.com/).
2.  Create a new project.
3.  In the navigation menu, go to "APIs & Services" > "Enabled APIs & services" and click **"+ ENABLE APIS AND SERVICES"**.
4.  Search for and enable the **"Google Drive API"**.
5.  Go to "APIs & Services" > "OAuth consent screen". Choose **"External"** and fill in the required application details (app name, user support email, developer contact). You do not need to submit for verification for personal use.
6.  Go to "APIs & Services" > "Credentials". Click **"+ CREATE CREDENTIALS"** and select **"OAuth client ID"**.
7.  Choose **"Desktop app"** as the application type.
8.  After creation, click the **"DOWNLOAD JSON"** button. Rename this file to `credentials.json` and place it in a `data/credentials/` directory within the project root.

### 2. Building the Project

```bash
# Clone the repository
git clone [https://github.com/your-username/D-Drive.git](https://github.com/your-username/D-Drive.git)
cd D-Drive

# Install dependencies using vcpkg
vcpkg install nlohmann-json cpr indicators httplib

# Configure the project with CMake, pointing it to the vcpkg toolchain
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake

# Build the project
cmake --build build

# The executable will be in the build/Debug or build/Release directory