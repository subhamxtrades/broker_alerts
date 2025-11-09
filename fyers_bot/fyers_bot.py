import os
from fyers_apiv3 import fyersModel

# --- Configuration ---
CLIENT_ID = os.environ.get("FYERS_CLIENT_ID", "YOUR_CLIENT_ID")
SECRET_KEY = os.environ.get("FYERS_SECRET_KEY", "YOUR_SECRET_KEY")
REDIRECT_URI = os.environ.get("FYERS_REDIRECT_URI", "https://trade.fyers.in/api-login/redirect-uri/index.html")

# --- Fyers API Integration ---

def create_session():
    """Creates and returns a Fyers SessionModel instance."""
    return fyersModel.SessionModel(
        client_id=CLIENT_ID,
        secret_key=SECRET_KEY,
        redirect_uri=REDIRECT_URI,
        response_type="code",
        grant_type="authorization_code"
    )

def generate_auth_code_url(session):
    """Generates and returns the authorization URL from a session."""
    return session.generate_authcode()

async def get_access_token(session, auth_code):
    """Generates an access token using the provided auth code and session."""
    session.set_token(auth_code)
    response = await session.generate_token()
    if response and "access_token" in response:
        return response["access_token"]
    else:
        print(f"Error getting access token: {response}")
        return None

def create_fyers_client(access_token):
    """Creates and returns a FyersModel (client) instance."""
    return fyersModel.FyersModel(
        client_id=CLIENT_ID,
        is_async=True,
        token=access_token,
        log_path=os.getcwd()
    )

async def get_pnl(fyers):
    """Fetches and returns the daily P&L."""
    try:
        positions = await fyers.positions()
        if positions and positions.get("netPositions"):
            total_pnl = sum(pos.get("unrealized_profit", 0) for pos in positions["netPositions"])
            return f"Your daily P&L is: {total_pnl:.2f}"
        else:
            return "No open positions found."
    except Exception as e:
        return f"An error occurred while fetching P&L: {e}"

async def get_orders(fyers):
    """Fetches and returns the order book."""
    try:
        order_book = await fyers.orderbook()
        if order_book and order_book.get("orderBook"):
            message = "Your Order Book:\n"
            for order in order_book["orderBook"]:
                message += f"Symbol: {order['symbol']}, Qty: {order['qty']}, Status: {order['status']}\n"
            return message
        else:
            return "No orders found in the order book."
    except Exception as e:
        return f"An error occurred while fetching orders: {e}"
