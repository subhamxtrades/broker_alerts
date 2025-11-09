# Fyers and Kotak Neo Telegram Bot

This is a Telegram bot that integrates with the Fyers and Kotak Neo trading APIs. It allows you to fetch your daily P&L and retrieve your order book.

## Setup

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/your-username/your-repo-name.git
    cd your-repo-name
    ```

2.  **Create and activate a virtual environment:**
    ```bash
    python3 -m venv venv
    source venv/bin/activate
    ```

3.  **Install the dependencies:**
    ```bash
    pip install -r requirements.txt
    ```

4.  **Set up your environment variables:**
    Create a `.env` file in the root of the project and add the following variables:
    ```
    FYERS_CLIENT_ID="your_fyers_client_id"
    FYERS_SECRET_KEY="your_fyers_secret_key"
    FYERS_REDIRECT_URI="your_fyers_redirect_uri"
    KOTAK_CONSUMER_KEY="your_kotak_consumer_key"
    KOTAK_CONSUMER_SECRET="your_kotak_consumer_secret"
    TELEGRAM_BOT_TOKEN="your_telegram_bot_token"
    ```

## Running the Bot

To run the bot, execute the following command:
```bash
python main_bot.py
```

Then, open Telegram and start a conversation with your bot.

## Security Warning

The Kotak Neo authentication process requires you to enter your mobile number, UCC, TOTP, and MPIN into the Telegram chat. Please be aware that this is not a secure method of authentication and could expose your account to risk. Use this feature at your own discretion.
