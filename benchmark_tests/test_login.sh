#!/bin/bash

# test_login.sh

# Base URL of the API
BASE_URL="http://localhost:3000"

# Endpoint for user login
ENDPOINT="/auth/login"

# Path to the JSON payload
PAYLOAD_FILE="benchmark_tests/login_payload.json"

# ab command
ab -n 1000 -c 100 -T "application/json" -p "$PAYLOAD_FILE" "$BASE_URL$ENDPOINT"
