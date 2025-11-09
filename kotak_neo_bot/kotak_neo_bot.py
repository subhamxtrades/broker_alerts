import os
from neo_api_client import NeoAPI

# --- Configuration ---
CONSUMER_KEY = os.environ.get("KOTAK_CONSUMER_KEY", "YOUR_CONSUMER_KEY")
CONSUMER_SECRET = os.environ.get("KOTAK_CONSUMER_SECRET", "YOUR_CONSUMER_SECRET")

# --- Kotak Neo API Integration ---

def get_neo_instance():
    """Initializes and returns a NeoAPI instance."""
    return NeoAPI(consumer_key=CONSUMER_KEY, consumer_secret=CONSUMER_SECRET, environment='prod')

def totp_login(client, mobile_number, ucc, totp):
    """Handles the TOTP login process."""
    client.totp_login(mobile_number=mobile_number, ucc=ucc, totp=totp)

def totp_validate(client, mpin):
    """Handles the TOTP validation process."""
    client.totp_validate(mpin=mpin)


def get_pnl(neo):
    """Fetches and returns the daily P&L."""
    try:
        positions = neo.positions()
        if positions and positions.get("data"):
            total_pnl = sum(float(pos.get("unrealised_pnl", 0)) for pos in positions["data"])
            return f"Your daily P&L is: {total_pnl:.2f}"
        else:
            return "No open positions found."
    except Exception as e:
        return f"An error occurred while fetching P&L: {e}"

def get_orders(neo):
    """Fetches and returns the order book."""
    try:
        order_book = neo.order_report()
        if order_book and order_book.get("data"):
            message = "Your Order Book:\n"
            for order in order_book["data"]:
                message += f"Symbol: {order['trading_symbol']}, Qty: {order['quantity']}, Status: {order['status']}\n"
            return message
        else:
            return "No orders found in the order book."
    except Exception as e:
        return f"An error occurred while fetching orders: {e}"
