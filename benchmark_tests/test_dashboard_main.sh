#!/bin/bash

# test_dashboard_main.sh

# Base URL of the API
BASE_URL="http://localhost:3000"

# Endpoint for the dashboard main listing
ENDPOINT="/dashboard/main"

# Query parameters
USER_ID="123"
START_DATE="2024-01-01"
END_DATE="2024-12-31"

# ab command
ab -n 1000 -c 100 "$BASE_URL$ENDPOINT?userId=$USER_ID&start_date=$START_DATE&end_date=$END_DATE"
