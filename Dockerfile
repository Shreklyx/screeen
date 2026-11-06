FROM node:22-bookworm-slim
WORKDIR /app
ENV NODE_ENV=production \
    PORT=8080
COPY package*.json ./
RUN npm ci --omit=dev && npm cache clean --force
COPY server ./server
COPY public ./public
# the permanent rooms and the key that signs rooms' tokens (a volume: they outlast an update, a restart)
RUN mkdir -p /data && chown node:node /data
ENV DATA_DIR=/data
USER node
EXPOSE 8080 3479/udp
CMD ["node", "server/server.js"]
