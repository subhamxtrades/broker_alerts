#!/bin/bash

# test_income_create.sh

# Base URL of the API
BASE_URL="http://localhost:3000"

# Endpoint for creating income
ENDPOINT="/income/create"

# Path to the JSON payload
PAYLOAD_FILE="benchmark_tests/income_payload.json"

# ab command
ab -n 500 -c 50 -T "application/json" -p "$PAYLOAD_FILE" "$BASE_URL$ENDPOINT"
