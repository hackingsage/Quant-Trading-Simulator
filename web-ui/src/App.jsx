import { useEffect, useState } from "react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  Tooltip,
  ResponsiveContainer,
} from "recharts";
import "./App.css";

function App() {
  const [connected, setConnected] = useState(false);
  const [ws, setWs] = useState(null);

  const [tob, setTob] = useState({
    bidPrice: 0,
    bidQty: 0,
    askPrice: 0,
    askQty: 0,
  });

  const [bids, setBids] = useState([]);
  const [asks, setAsks] = useState([]);
  const [trades, setTrades] = useState([]);
  const [lastOrderId, setLastOrderId] = useState(null);

  const [formSide, setFormSide] = useState("buy");
  const [formPrice, setFormPrice] = useState("100");
  const [formQty, setFormQty] = useState("10");

  const [priceSeries, setPriceSeries] = useState([]);

  const [pnl, setPnl] = useState({
    realized: 0,
    unrealized: 0,
    position: 0,
    avg_price: 0,
    equity: 0,
  });

  // NEW: BS bot PnL
  const [bsPnl, setBsPnl] = useState({
    realized: 0,
    unrealized: 0,
    position: 0,
    avg_price: 0,
    equity: 0,
  });

  // --- WebSocket setup ---
  useEffect(() => {
    const wsUrl = `ws://${window.location.hostname}:8080`;
    const socket = new WebSocket(wsUrl);

    socket.onopen = () => {
      console.log("WS connected");
      setConnected(true);
    };

    socket.onclose = () => {
      console.log("WS disconnected");
      setConnected(false);
    };

    socket.onmessage = (event) => {
      const msg = JSON.parse(event.data);
      handleServerMessage(msg);
    };

    setWs(socket);

    return () => {
      socket.close();
    };
  }, []);

  // -------- Classification Logic (Corrected) --------
  function classify(id) {
    if (!Number.isFinite(id)) return { label: "UNKNOWN", kind: "unknown" };
    if (id === 0) return { label: "SYSTEM", kind: "system" };
    if (id >= 9000) return { label: `BOT-${id}`, kind: "bot" };
    return { label: `USER-${id}`, kind: "user" };
  }

  // helper to merge PnL updates safely
  function applyPnlUpdate(prev, msg) {
    return {
      realized:
        typeof msg.realized === "number" ? msg.realized : prev.realized,
      unrealized:
        typeof msg.unrealized === "number" ? msg.unrealized : prev.unrealized,
      position:
        typeof msg.position === "number" ? msg.position : prev.position,
      avg_price:
        typeof msg.avg_price === "number" ? msg.avg_price : prev.avg_price,
      equity:
        typeof msg.equity === "number" ? msg.equity : prev.equity,
    };
  }

  // --- Message handling from backend ---
  function handleServerMessage(msg) {
    if (msg.type === "tob") {
      setTob({
        bidPrice: msg.bidPrice,
        bidQty: msg.bidQty,
        askPrice: msg.askPrice,
        askQty: msg.askQty,
      });
    }

    else if (msg.type === "l2_update") {
      if (msg.side === 0) {
        setBids((prev) => updateBookSide(prev, msg.price, msg.quantity, true));
      } else {
        setAsks((prev) => updateBookSide(prev, msg.price, msg.quantity, false));
      }
    }

    else if (msg.type === "trade") {
      const buyer = classify(msg.buy_user_id);
      const seller = classify(msg.sell_user_id);

      const t = {
        trade_id: msg.trade_id ?? Date.now(),
        price: msg.price,
        quantity: msg.quantity,

        buyer_id: msg.buy_user_id,
        seller_id: msg.sell_user_id,

        buyer_label: buyer.label,
        seller_label: seller.label,

        whoLabel: `${buyer.label} ↔ ${seller.label}`,
        isBotTrade: buyer.kind === "bot" || seller.kind === "bot",
        isSystemTrade: buyer.kind === "system" || seller.kind === "system",

        ts: new Date().toLocaleTimeString(),
      };

      // Add recent trade
      setTrades((prev) => [t, ...prev].slice(0, 200));

      // Add to price chart
      setPriceSeries((prev) => {
        const next = [...prev, { time: t.ts, price: t.price }];
        return next.slice(-300);
      });
    }

    else if (msg.type === "ack") {
      setLastOrderId(msg.order_id);
    }

    else if (msg.type === "pnl") {
      const uid = msg.user_id ?? 0;

      // BS bot (user_id >= 9000)
      if (uid >= 9000) {
        setBsPnl((prev) => applyPnlUpdate(prev, msg));
      } else {
        // manual user (default behaviour)
        setPnl((prev) => applyPnlUpdate(prev, msg));
      }
    }
  }

  function updateBookSide(levels, price, quantity, isBid) {
    const p = Number(price);
    const q = Number(quantity);
    let found = false;

    let next = levels.map((lvl) => {
      if (lvl.price === p) {
        found = true;
        return { price: p, quantity: q };
      }
      return lvl;
    });

    if (!found && q > 0) {
      next.push({ price: p, quantity: q });
    }

    next = next.filter((lvl) => lvl.quantity > 0);

    next.sort((a, b) => (isBid ? b.price - a.price : a.price - b.price));
    return next;
  }

  // ---- Order Submission ----
  function sendOrder(e) {
    e.preventDefault();
    if (!ws || ws.readyState !== WebSocket.OPEN) return;

    ws.send(
      JSON.stringify({
        type: "new_order",
        user_id: 1,
        side: formSide,
        price: parseFloat(formPrice),
        quantity: parseInt(formQty, 10),
      })
    );
  }

  const midPrice =
    tob.bidPrice > 0 && tob.askPrice > 0
      ? (tob.bidPrice + tob.askPrice) / 2
      : tob.bidPrice || tob.askPrice || 0;

  return (
    <div className="app-root">
      {/* --- Top Gradient --- */}
      <div className="app-gradient" />

      {/* --- Header --- */}
      <header className="app-header">
        <div className="header-left">
          <div className="app-logo-circle">Q</div>
          <div>
            <h1 className="app-title">Quant Trading Simulator</h1>
            <p className="app-subtitle">
              Monte Carlo driven matching engine • Live market microstructure
            </p>
          </div>
        </div>

        <div className="header-right">
          <div
            className={`conn-pill ${
              connected ? "conn-pill-online" : "conn-pill-offline"
            }`}
          >
            <span className="conn-dot" />
            {connected ? "Live connection" : "Disconnected"}
          </div>
        </div>
      </header>

      {/* Main layout */}
      <main className="app-main">
        {/* LEFT COLUMN - Trade ticket + PnL */}
        <section className="card card-tall">
          <div className="card-header">
            <h2>Trade Ticket</h2>
            <span className="card-tag">Manual trader · user 1</span>
          </div>

          <form onSubmit={sendOrder} className="ticket-form">
            <div className="ticket-side-toggle">
              <button
                type="button"
                className={`side-btn ${
                  formSide === "buy" ? "side-btn-active-buy" : ""
                }`}
                onClick={() => setFormSide("buy")}
              >
                Buy
              </button>

              <button
                type="button"
                className={`side-btn ${
                  formSide === "sell" ? "side-btn-active-sell" : ""
                }`}
                onClick={() => setFormSide("sell")}
              >
                Sell
              </button>
            </div>

            <div className="ticket-grid">
              <label className="field">
                <span className="field-label">Price</span>
                <input
                  type="number"
                  step="0.01"
                  value={formPrice}
                  onChange={(e) => setFormPrice(e.target.value)}
                />
              </label>

              <label className="field">
                <span className="field-label">Quantity</span>
                <input
                  type="number"
                  min="1"
                  value={formQty}
                  onChange={(e) => setFormQty(e.target.value)}
                />
              </label>

              <div className="field readonly-field">
                <span className="field-label">Mid Price</span>
                <div className="field-value">
                  {midPrice ? midPrice.toFixed(2) : "—"}
                </div>
              </div>
            </div>

            <button type="submit" disabled={!connected} className="submit-btn">
              {formSide === "buy" ? "Place Buy Order" : "Place Sell Order"}
            </button>
          </form>

          <div className="ticket-footer">
            <span className="footer-label">Last ACK order id</span>
            <span className="footer-value">
              {lastOrderId != null ? lastOrderId : "—"}
            </span>
          </div>

          <div className="divider" />

          {/* PnL section */}
          <div className="card-header">
            <h2>Your PnL</h2>
            <span className="card-tag pill-soft">Mark-to-market</span>
          </div>

          <div className="pnl-grid">
            <div className="pnl-item">
              <span className="pnl-label">Position</span>
              <span className="pnl-value">{pnl.position.toFixed(2)}</span>
            </div>
            <div className="pnl-item">
              <span className="pnl-label">Avg Price</span>
              <span className="pnl-value">
                {pnl.avg_price ? pnl.avg_price.toFixed(2) : "—"}
              </span>
            </div>
            <div className="pnl-item">
              <span className="pnl-label">Realized</span>
              <span
                className={`pnl-value ${
                  pnl.realized >= 0 ? "pnl-pos" : "pnl-neg"
                }`}
              >
                {pnl.realized.toFixed(2)}
              </span>
            </div>
            <div className="pnl-item">
              <span className="pnl-label">Unrealized</span>
              <span
                className={`pnl-value ${
                  pnl.unrealized >= 0 ? "pnl-pos" : "pnl-neg"
                }`}
              >
                {pnl.unrealized.toFixed(2)}
              </span>
            </div>
            <div className="pnl-item pnl-span-2">
              <span className="pnl-label">Equity</span>
              <span
                className={`pnl-value pnl-big ${
                  pnl.equity >= 0 ? "pnl-pos" : "pnl-neg"
                }`}
              >
                {pnl.equity.toFixed(2)}
              </span>
            </div>
          </div>

          {/* --- NEW: BS Bot PnL panel --- */}
          <div className="divider" />

          <div className="card-header">
            <h2>BS Bot PnL</h2>
            <span className="card-tag pill-soft">Options + Hedge</span>
          </div>

          <div className="pnl-grid">
            <div className="pnl-item">
              <span className="pnl-label">Position</span>
              <span className="pnl-value">{bsPnl.position.toFixed(2)}</span>
            </div>
            <div className="pnl-item">
              <span className="pnl-label">Avg Price</span>
              <span className="pnl-value">
                {bsPnl.avg_price ? bsPnl.avg_price.toFixed(2) : "—"}
              </span>
            </div>
            <div className="pnl-item">
              <span className="pnl-label">Realized</span>
              <span
                className={`pnl-value ${
                  bsPnl.realized >= 0 ? "pnl-pos" : "pnl-neg"
                }`}
              >
                {bsPnl.realized.toFixed(2)}</span>
            </div>
            <div className="pnl-item">
              <span className="pnl-label">Unrealized</span>
              <span
                className={`pnl-value ${
                  bsPnl.unrealized >= 0 ? "pnl-pos" : "pnl-neg"
                }`}
              >
                {bsPnl.unrealized.toFixed(2)}
              </span>
            </div>
            <div className="pnl-item pnl-span-2">
              <span className="pnl-label">Equity</span>
              <span
                className={`pnl-value pnl-big ${
                  bsPnl.equity >= 0 ? "pnl-pos" : "pnl-neg"
                }`}
              >
                {bsPnl.equity.toFixed(2)}
              </span>
            </div>
          </div>
        </section>

        {/* MIDDLE COLUMN - Order book */}
        <section className="card card-tall">
          <div className="card-header">
            <h2>Order Book</h2>
            <span className="card-tag">Level 2 depth</span>
          </div>

          <div className="book-wrapper">
            <div className="book-side-column">
              <div className="book-side-header">
                <span>Bids</span>
                <span className="side-label bid-label">Bid</span>
              </div>
              <div className="book-table-wrapper">
                <table className="book-table">
                  <thead>
                    <tr>
                      <th>Price</th>
                      <th>Qty</th>
                    </tr>
                  </thead>
                  <tbody>
                    {bids.length === 0 && (
                      <tr>
                        <td colSpan={2} className="empty-cell">
                          No bids yet
                        </td>
                      </tr>
                    )}
                    {bids.map((b) => (
                      <tr key={b.price}>
                        <td className="bid">{b.price.toFixed(2)}</td>
                        <td>{b.quantity}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </div>

            <div className="book-side-column">
              <div className="book-side-header">
                <span>Asks</span>
                <span className="side-label ask-label">Ask</span>
              </div>
              <div className="book-table-wrapper">
                <table className="book-table">
                  <thead>
                    <tr>
                      <th>Price</th>
                      <th>Qty</th>
                    </tr>
                  </thead>
                  <tbody>
                    {asks.length === 0 && (
                      <tr>
                        <td colSpan={2} className="empty-cell">
                          No asks yet
                        </td>
                      </tr>
                    )}
                    {asks.map((a) => (
                      <tr key={a.price}>
                        <td className="ask">{a.price.toFixed(2)}</td>
                        <td>{a.quantity}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </div>
          </div>
        </section>

        {/* RIGHT COLUMN - Chart + Trades */}
        <section className="card card-tall">
          <div className="card-header">
            <h2>Price Action</h2>
            <span className="card-tag">Last trades</span>
          </div>

          <div className="chart-container">
            {priceSeries.length === 0 ? (
              <div className="empty-chart">Waiting for trades…</div>
            ) : (
              <ResponsiveContainer width="100%" height="100%">
                <LineChart data={priceSeries}>
                  <XAxis dataKey="time" hide />
                  <YAxis domain={["auto", "auto"]} />
                  <Tooltip
                    contentStyle={{
                      backgroundColor: "#020617",
                      border: "1px solid #1f2937",
                      borderRadius: "8px",
                      fontSize: "0.75rem",
                    }}
                  />
                  <Line
                    type="monotone"
                    dataKey="price"
                    strokeWidth={2}
                    dot={false}
                  />
                </LineChart>
              </ResponsiveContainer>
            )}
          </div>

          <div className="card-header card-header-tight">
            <h2>Recent Trades</h2>
            <span className="card-tag pill-soft">
              {trades.length} shown
            </span>
          </div>

          <div className="trades-table-wrapper">
            <table className="trades-table">
              <thead>
                <tr>
                  <th>Time</th>
                  <th>Price</th>
                  <th>Qty</th>
                  <th>Who</th>
                </tr>
              </thead>
              <tbody>
                {trades.length === 0 && (
                  <tr>
                    <td colSpan={4} className="empty-cell">
                      No trades yet
                    </td>
                  </tr>
                )}

                {trades.map((t) => (
                  <tr key={`${t.trade_id}-${t.ts}`}>
                    <td>{t.ts}</td>
                    <td>{t.price.toFixed(4)}</td>
                    <td>{t.quantity}</td>
                    <td>
                      <span
                        className={
                          t.isBotTrade
                            ? "tag-bot"
                            : t.isSystemTrade
                            ? "tag-system"
                            : "tag-user"
                        }
                      >
                        {t.whoLabel}
                      </span>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

        </section>
      </main>
    </div>
  );
}

export default App;
