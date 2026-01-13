#!/bin/bash

# test_contact_list.sh

# Base URL of the API
BASE_URL="http://localhost:3000"

# Endpoint for listing contacts
ENDPOINT="/contact/list"

# Query parameters
USER_ID="123"
PAGE="1"
PAGE_SIZE="20"

# ab command
ab -n 1000 -c 100 "$BASE_URL$ENDPOINT?user_id=$USER_ID&page=$PAGE&pageSize=$PAGE_SIZE"
