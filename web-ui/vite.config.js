import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// LAN-ready config
export default defineConfig({
  plugins: [react()],
  server: {
    host: true,          // allow LAN access
    port: 5173,          // your frontend port
    strictPort: true,    // do not auto-switch ports
    cors: true,          // allow cross-origin WS
    allowedHosts: [
      "localhost",
      "127.0.0.1",
      ".ngrok-free.app"
    ],
  }
})
