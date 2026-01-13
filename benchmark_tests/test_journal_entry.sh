#!/bin/bash

# test_journal_entry.sh

# Base URL of the API
BASE_URL="http://localhost:3000"

# Endpoint for creating a journal entry
ENDPOINT="/accounting/journal-entries"

# Path to the JSON payload
PAYLOAD_FILE="benchmark_tests/journal_entry_payload.json"

# ab command
ab -n 500 -c 50 -T "application/json" -p "$PAYLOAD_FILE" "$BASE_URL$ENDPOINT"
