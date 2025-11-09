import logging
import os
import asyncio
from telegram import Update, ReplyKeyboardMarkup, ReplyKeyboardRemove
from telegram.ext import (
    Application,
    CommandHandler,
    ContextTypes,
    ConversationHandler,
    MessageHandler,
    filters,
)

from fyers_bot import fyers_bot
from kotak_neo_bot import kotak_neo_bot

# Enable logging
logging.basicConfig(
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s", level=logging.INFO
)
logger = logging.getLogger(__name__)

# --- State Definitions ---
SELECTING_PLATFORM, AUTHENTICATING, FYERS_ACTION, KOTAK_ACTION = range(4)

# --- Bot Commands ---
async def start(update: Update, context: ContextTypes.DEFAULT_TYPE) -> int:
    """Starts the conversation and asks for the user's choice."""
    reply_keyboard = [["Fyers", "Kotak Neo"]]
    await update.message.reply_text(
        "Hi! I am your trading bot. Please choose a platform to get started.",
        reply_markup=ReplyKeyboardMarkup(
            reply_keyboard, one_time_keyboard=True, input_field_placeholder="Fyers or Kotak Neo?"
        ),
    )
    return SELECTING_PLATFORM

async def platform_choice(update: Update, context: ContextTypes.DEFAULT_TYPE) -> int:
    """Handles the user's choice of platform."""
    platform = update.message.text.lower()
    context.user_data['platform'] = platform
    await update.message.reply_text(
        f"You've selected {platform.capitalize()}. Please authenticate to continue.",
        reply_markup=ReplyKeyboardRemove(),
    )
    if platform == 'fyers':
        session = fyers_bot.create_session()
        context.user_data['fyers_session'] = session
        auth_url = fyers_bot.generate_auth_code_url(session)
        await update.message.reply_text(f"Please login and get the auth code from this URL: {auth_url}")
        await update.message.reply_text("Please send me the auth code.")
    elif platform == 'kotak neo':
        await update.message.reply_text("Please send me your mobile number.")
    return AUTHENTICATING

async def received_authentication(update: Update, context: ContextTypes.DEFAULT_TYPE) -> int:
    """Handles the user's authentication credentials."""
    platform = context.user_data['platform']
    if platform == 'fyers':
        auth_code = update.message.text
        session = context.user_data['fyers_session']
        access_token = await fyers_bot.get_access_token(session, auth_code)
        if access_token:
            fyers = fyers_bot.create_fyers_client(access_token)
            context.user_data['fyers_client'] = fyers
            await update.message.reply_text("Authentication successful! What would you like to do?\n"
                                   "/pnl - Get P&L\n"
                                   "/orders - Get orders")
            return FYERS_ACTION
        else:
            await update.message.reply_text("Authentication failed. Please try again.")
            return ConversationHandler.END
    elif platform == 'kotak neo':
        if 'mobile_number' not in context.user_data:
            context.user_data['mobile_number'] = update.message.text
            await update.message.reply_text("Please send me your UCC.")
            return AUTHENTICATING
        elif 'ucc' not in context.user_data:
            context.user_data['ucc'] = update.message.text
            await update.message.reply_text("Please send me your TOTP.")
            return AUTHENTICATING
        elif 'totp' not in context.user_data:
            context.user_data['totp'] = update.message.text
            neo = kotak_neo_bot.get_neo_instance()
            await asyncio.to_thread(kotak_neo_bot.totp_login, neo, context.user_data['mobile_number'], context.user_data['ucc'], context.user_data['totp'])
            context.user_data['kotak_client'] = neo
            await update.message.reply_text("Please send me your MPIN.")
            return AUTHENTICATING
        else:
            mpin = update.message.text
            neo = context.user_data['kotak_client']
            await asyncio.to_thread(kotak_neo_bot.totp_validate, neo, mpin)
            await update.message.reply_text("Authentication successful! What would you like to do?\n"
                                   "/pnl - Get P&L\n"
                                   "/orders - Get orders")
            return KOTAK_ACTION

async def fyers_pnl(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    """Fetches and displays the daily P&L for Fyers."""
    fyers = context.user_data.get('fyers_client')
    if not fyers:
        await update.message.reply_text("Not authenticated with Fyers.")
        return
    pnl = await fyers_bot.get_pnl(fyers)
    await update.message.reply_text(pnl)

async def fyers_orders(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    """Fetches and displays the order book for Fyers."""
    fyers = context.user_data.get('fyers_client')
    if not fyers:
        await update.message.reply_text("Not authenticated with Fyers.")
        return
    orders = await fyers_bot.get_orders(fyers)
    await update.message.reply_text(orders)

async def kotak_pnl(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    """Fetches and displays the daily P&L for Kotak Neo."""
    neo = context.user_data.get('kotak_client')
    if not neo:
        await update.message.reply_text("Not authenticated with Kotak Neo.")
        return
    pnl = await asyncio.to_thread(kotak_neo_bot.get_pnl, neo)
    await update.message.reply_text(pnl)

async def kotak_orders(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    """Fetches and displays the order book for Kotak Neo."""
    neo = context.user_data.get('kotak_client')
    if not neo:
        await update.message.reply_text("Not authenticated with Kotak Neo.")
        return
    orders = await asyncio.to_thread(kotak_neo_bot.get_orders, neo)
    await update.message.reply_text(orders)

async def cancel(update: Update, context: ContextTypes.DEFAULT_TYPE) -> int:
    """Cancels and ends the conversation."""
    await update.message.reply_text("Bye! I hope we can talk again some day.")
    return ConversationHandler.END

# --- Main Function ---
def main() -> None:
    """Run the bot."""
    # Create the Application and pass it your bot's token.
    application = Application.builder().token(os.environ.get("TELEGRAM_BOT_TOKEN")).build()

    conv_handler = ConversationHandler(
        entry_points=[CommandHandler("start", start)],
        states={
            SELECTING_PLATFORM: [
                MessageHandler(filters.Regex("^(Fyers|Kotak Neo)$"), platform_choice)
            ],
            AUTHENTICATING: [
                MessageHandler(filters.TEXT & ~filters.COMMAND, received_authentication)
            ],
            FYERS_ACTION: [
                CommandHandler("pnl", fyers_pnl),
                CommandHandler("orders", fyers_orders),
            ],
            KOTAK_ACTION: [
                CommandHandler("pnl", kotak_pnl),
                CommandHandler("orders", kotak_orders),
            ],
        },
        fallbacks=[CommandHandler("cancel", cancel)],
    )

    application.add_handler(conv_handler)

    # Run the bot until the user presses Ctrl-C
    application.run_polling()


if __name__ == "__main__":
    main()
