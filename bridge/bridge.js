const net = require("net");
const WebSocket = require("ws");

const ENGINE_HOST = "127.0.0.1";
const ENGINE_PORT = 9001;
const WS_PORT = 8080;

let engineSocket = null;
let engineBuffer = Buffer.alloc(0);
let wsClients = new Set();

/**
 * Connect to the C++ engine over TCP and set up framed message handling.
 * Reconnects automatically on close with a short delay.
 */
function connectEngine() {
  if (engineSocket) return;

  console.log(`[bridge] Connecting to engine ${ENGINE_HOST}:${ENGINE_PORT}...`);

  engineSocket = net.createConnection(
    { host: ENGINE_HOST, port: ENGINE_PORT },
    () => console.log("[bridge] Connected to engine.")
  );

  engineSocket.on("data", (chunk) => {
    engineBuffer = Buffer.concat([engineBuffer, chunk]);
    processEngineBuffer();
  });

  engineSocket.on("error", (err) =>
    console.error("[bridge] Engine error:", err.message)
  );

  engineSocket.on("close", () => {
    console.log("[bridge] Engine disconnected, retrying...");
    engineSocket = null;
    setTimeout(connectEngine, 1000);
  });
}

// ------------------------------------------------------------
// Frame Decoder
// ------------------------------------------------------------
/**
 * Deframe engine TCP stream: frames are [4-byte BE length][payload].
 * Processes available complete payloads, buffering partial data.
 */
function processEngineBuffer() {
  while (engineBuffer.length >= 4) {
    const length = engineBuffer.readUInt32BE(0);
    if (engineBuffer.length < 4 + length) break;

    const payload = engineBuffer.slice(4, 4 + length);
    engineBuffer = engineBuffer.slice(4 + length);

    handleEngineMessage(payload);
  }
}

// ------------------------------------------------------------
// Payload Decoder
// ------------------------------------------------------------
/**
 * Decode a single engine payload and broadcast normalized JSON over WebSocket.
 * Supported frame types:
 * 3: trade, 4: ack, 5: top of book, 6: L2 update, 7: PnL update
 * @param {Buffer} payload - raw payload without length prefix
 */
function handleEngineMessage(payload) {
  const type = payload.readUInt8(0);

  // -------------------------
  // TRADE FRAME (type = 3)
  // -------------------------
  if (type === 3) {
    let offset = 1;

    const trade_id      = Number(payload.readBigUInt64BE(offset)); offset += 8;
    const buy_order_id  = Number(payload.readBigUInt64BE(offset)); offset += 8;
    const buy_user_id   = Number(payload.readBigUInt64BE(offset)); offset += 8;
    const sell_order_id = Number(payload.readBigUInt64BE(offset)); offset += 8;
    const sell_user_id  = Number(payload.readBigUInt64BE(offset)); offset += 8;
    const price         = payload.readDoubleBE(offset); offset += 8;
    const quantity      = Number(payload.readBigUInt64BE(offset)); offset += 8;

    broadcastJSON({
      type: "trade",
      trade_id,
      buy_order_id,
      sell_order_id,
      buy_user_id,
      sell_user_id,
      price,
      quantity
    });
  }

  // -------------------------
  // ACK FRAME (type = 4)
  // -------------------------
  else if (type === 4) {
    const status   = payload.readUInt8(1);
    const ackType  = payload.readUInt8(2);
    const order_id = Number(payload.readBigUInt64BE(3));

    broadcastJSON({
      type: "ack",
      status,
      ackType,
      order_id
    });
  }

  // -------------------------
  // TOP OF BOOK FRAME (type = 5)
  // -------------------------
  else if (type === 5) {
    let offset = 1;

    const bidPrice = payload.readDoubleBE(offset); offset += 8;
    const bidQty   = Number(payload.readBigUInt64BE(offset)); offset += 8;
    const askPrice = payload.readDoubleBE(offset); offset += 8;
    const askQty   = Number(payload.readBigUInt64BE(offset));

    broadcastJSON({
      type: "tob",
      bidPrice,
      bidQty,
      askPrice,
      askQty
    });
  }

  // -------------------------
  // L2_UPDATE (type = 6)
  // -------------------------
  else if (type === 6) {
    let offset = 1;

    const side     = payload.readUInt8(offset); offset += 1;
    const price    = payload.readDoubleBE(offset); offset += 8;
    const quantity = Number(payload.readBigUInt64BE(offset));

    broadcastJSON({
      type: "l2_update",
      side,
      price,
      quantity
    });
  }

  // -------------------------
  // PNL_UPDATE (type = 7)
  // -------------------------
  else if (type === 7) {
    let offset = 1;

    const user_id = payload.readUInt32BE(offset); offset += 4;
    const realized   = payload.readDoubleBE(offset); offset += 8;
    const unrealized = payload.readDoubleBE(offset); offset += 8;
    // const position   = Number(payload.readBigUInt64BE(offset)); offset += 8;
    const position = payload.readDoubleBE(offset); offset += 8;
    const avg_price  = payload.readDoubleBE(offset); offset += 8;
    const equity     = payload.readDoubleBE(offset);

    broadcastJSON({
      type: "pnl",
      user_id,
      realized,
      unrealized,
      position,
      avg_price,
      equity
    });
  }
}

// ------------------------------------------------------------
// Outbound messaging
// ------------------------------------------------------------
/**
 * Broadcast a JSON-serializable object to all connected WebSocket clients.
 * @param {any} obj
 */
function broadcastJSON(obj) {
  const msg = JSON.stringify(obj);
  for (const c of wsClients) {
    if (c.readyState === WebSocket.OPEN) c.send(msg);
  }
}

// ------------------------------------------------------------
// Client → Engine API
// ------------------------------------------------------------
/**
 * Send a NEW_ORDER frame to the engine.
 * @param {{user_id:number, side:0|1, price:number, quantity:number}} param0
 */
function sendNewOrderToEngine({ user_id, side, price, quantity }) {
  if (!engineSocket) return;

  const payload = Buffer.alloc(1 + 8 + 1 + 8 + 8);
  let offset = 0;

  payload.writeUInt8(1, offset); offset += 1;               // NEW_ORDER
  payload.writeBigUInt64BE(BigInt(user_id), offset); offset += 8;
  payload.writeUInt8(side, offset); offset += 1;
  payload.writeDoubleBE(price, offset); offset += 8;
  payload.writeBigUInt64BE(BigInt(quantity), offset); offset += 8;

  const frame = Buffer.alloc(4 + payload.length);
  frame.writeUInt32BE(payload.length, 0);
  payload.copy(frame, 4);

  engineSocket.write(frame);
}

/**
 * Send a CANCEL frame to the engine.
 * @param {{order_id:number}} param0
 */
function sendCancelToEngine({ order_id }) {
  if (!engineSocket) return;

  const payload = Buffer.alloc(1 + 8);
  payload.writeUInt8(2, 0); // CANCEL
  payload.writeBigUInt64BE(BigInt(order_id), 1);

  const frame = Buffer.alloc(4 + payload.length);
  frame.writeUInt32BE(payload.length, 0);
  payload.copy(frame, 4);

  engineSocket.write(frame);
}

// ------------------------------------------------------------
// Boot
// ------------------------------------------------------------
connectEngine();

const wss = new WebSocket.Server(
  { host: "0.0.0.0", port: WS_PORT },
  () => console.log(`[bridge] Websocket ready ws://0.0.0.0:${WS_PORT}`)
);

wss.on("connection", (ws) => {
  wsClients.add(ws);
  console.log("[bridge] Web client connected");

  ws.on("message", (raw) => {
    let data;
    try { data = JSON.parse(raw.toString()); }
    catch (e) { return; }

    if (data.type === "new_order") {
      sendNewOrderToEngine({
        user_id: data.user_id ?? 1,
        side: data.side === "buy" ? 0 : 1,
        price: Number(data.price),
        quantity: Number(data.quantity)
      });
    }

    if (data.type === "cancel") {
      sendCancelToEngine({ order_id: Number(data.order_id) });
    }
  });

  ws.on("close", () => wsClients.delete(ws));
});
