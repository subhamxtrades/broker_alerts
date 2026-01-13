# Benchmark Test Plan

## Introduction

This document outlines the plan for benchmarking the API using Apache Bench (`ab`). The goal is to measure the performance of key API endpoints under load to identify potential bottlenecks and establish a performance baseline.

## Test Environment

- **Tool:** Apache Bench (`ab`)
- **Host:** The base URL of the API will be configurable (e.g., `http://localhost:3000`).

## Selected Endpoints for Benchmarking

The following endpoints have been selected to represent a mix of common and critical operations:

### 1. Dashboard Module

- **Endpoint:** `GET /dashboard/main`
- **Description:** Fetches the main dashboard data. This is a high-frequency read operation that is critical to the user experience.
- **Test Scenario:** Simulate 1000 requests with a concurrency of 100.

### 2. Accounting Module

- **Endpoint:** `POST /accounting/journal-entries`
- **Description:** Creates a new journal entry. This is a fundamental write operation in the accounting module.
- **Test Scenario:** Simulate 500 requests with a concurrency of 50, using a sample JSON payload.

### 3. Profile Management Module

- **Endpoint:** `POST /auth/login`
- **Description:** Authenticates a user. This is a critical endpoint for all users.
- **Test Scenario:** Simulate 1000 requests with a concurrency of 100, using a sample JSON payload for login credentials.

### 4. Contact Module

- **Endpoint:** `GET /contact/list`
- **Description:** Lists contacts with filtering and pagination. This represents a common read operation with multiple parameters.
- **Test Scenario:** Simulate 1000 requests with a concurrency of 100.

### 5. Income Module

- **Endpoint:** `POST /income/create`
- **Description:** Creates a new income entry. This is a complex write operation that involves multiple relationships.
- **Test Scenario:** Simulate 500 requests with a concurrency of 50, using a sample JSON payload.

## Execution and Reporting

The benchmark tests will be executed via a master script that runs each `ab` test sequentially. The output of each test will be logged to a consolidated report file, `benchmark_report.txt`. This report will contain performance metrics such as requests per second, time per request, and connection times for each of the selected endpoints.
