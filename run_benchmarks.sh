#!/bin/bash

# run_benchmarks.sh
# This script executes all benchmark tests and generates a consolidated report.

REPORT_FILE="benchmark_report.txt"

# Clear the report file if it already exists
> $REPORT_FILE

echo "Starting benchmark tests..." | tee -a $REPORT_FILE
echo "=============================" | tee -a $REPORT_FILE
echo "" | tee -a $REPORT_FILE

# --- Test: GET /dashboard/main ---
echo "Running test: GET /dashboard/main" | tee -a $REPORT_FILE
bash benchmark_tests/test_dashboard_main.sh >> $REPORT_FILE
echo "" | tee -a $REPORT_FILE
echo "-----------------------------" | tee -a $REPORT_FILE
echo "" | tee -a $REPORT_FILE

# --- Test: POST /accounting/journal-entries ---
echo "Running test: POST /accounting/journal-entries" | tee -a $REPORT_FILE
bash benchmark_tests/test_journal_entry.sh >> $REPORT_FILE
echo "" | tee -a $REPORT_FILE
echo "-----------------------------" | tee -a $REPORT_FILE
echo "" | tee -a $REPORT_FILE

# --- Test: POST /auth/login ---
echo "Running test: POST /auth/login" | tee -a $REPORT_FILE
bash benchmark_tests/test_login.sh >> $REPORT_FILE
echo "" | tee -a $REPORT_FILE
echo "-----------------------------" | tee -a $REPORT_FILE
echo "" | tee -a $REPORT_FILE

# --- Test: GET /contact/list ---
echo "Running test: GET /contact/list" | tee -a $REPORT_FILE
bash benchmark_tests/test_contact_list.sh >> $REPORT_FILE
echo "" | tee -a $REPORT_FILE
echo "-----------------------------" | tee -a $REPORT_FILE
echo "" | tee -a $REPORT_FILE

# --- Test: POST /income/create ---
echo "Running test: POST /income/create" | tee -a $REPORT_FILE
bash benchmark_tests/test_income_create.sh >> $REPORT_FILE
echo "" | tee -a $REPORT_FILE
echo "-----------------------------" | tee -a $REPORT_FILE
echo "" | tee -a $REPORT_FILE

echo "Benchmark tests complete. Report generated at: $REPORT_FILE" | tee -a $REPORT_FILE
