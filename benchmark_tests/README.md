# API Benchmark Test Suite

## Introduction

This directory contains a set of scripts designed to benchmark the web API using Apache Bench (`ab`). These tests will help measure the performance of key API endpoints under load.

## Prerequisites

1.  **Apache Bench (`ab`)**: You must have `ab` installed.
    - On **Debian-based systems (like Ubuntu)**, you can install it with:
      ```bash
      sudo apt-get update && sudo apt-get install -y apache2-utils
      ```
    - On **CentOS 9**, you can install it with:
      ```bash
      sudo dnf install -y httpd-tools
      ```
2.  **Running API Server**: The API server must be running and accessible from the machine where you intend to run these tests.

## Configuration

### Base URL

Before running any tests, you must configure the `BASE_URL` variable inside each test script (`.sh` file) to point to your running API.

For example, open `test_dashboard_main.sh` and change the following line:
```sh
# Base URL of the API
BASE_URL="http://localhost:3000"
```
Replace `"http://localhost:3000"` with the actual base URL of your API instance.

### Payloads

The `POST` request tests rely on JSON payload files (e.g., `login_payload.json`). You can modify the data in these files to suit your testing needs.

## How to Run the Tests

You can run the entire test suite at once or execute individual tests. It is recommended to run the scripts from the **root directory** of the repository to ensure the payload paths are resolved correctly.

### 1. Run the Entire Suite

A master script, `run_benchmarks.sh`, is provided in the root directory to execute all tests sequentially. It will generate a consolidated `benchmark_report.txt` file with the results.

```bash
# From the root directory of the repository
chmod +x run_benchmarks.sh
./run_benchmarks.sh
```

### 2. Run Individual Tests

You can also run any test script individually.

```bash
# Example: Running the login test from the root directory
chmod +x benchmark_tests/test_login.sh
./benchmark_tests/test_login.sh
```

## Included Test Scripts

- `test_dashboard_main.sh`: Benchmarks the `GET /dashboard/main` endpoint.
- `test_journal_entry.sh`: Benchmarks the `POST /accounting/journal-entries` endpoint.
- `test_login.sh`: Benchmarks the `POST /auth/login` endpoint.
- `test_contact_list.sh`: Benchmarks the `GET /contact/list` endpoint.
- `test_income_create.sh`: Benchmarks the `POST /income/create` endpoint.
