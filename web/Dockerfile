# Build stage
FROM node:21-alpine AS build
WORKDIR /app
COPY package*.json ./
RUN npm install
COPY . .
RUN npm run build

# Runtime stage
FROM nginx:1.25-alpine
COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY cloudflare-realip.conf /etc/nginx/cloudflare-realip.conf
COPY edge-auth.conf /etc/nginx/edge-auth.conf
COPY docker-entrypoint.sh /docker-entrypoint.sh
COPY --from=build /app/dist /usr/share/nginx/html
# Where nginx listens: docker-entrypoint.sh writes it at every start (443 with
# the origin certificate, 80 without); this default lets `nginx -t` read the
# image's configuration as it stands.
RUN printf 'listen 80;\n' > /etc/nginx/listen.conf
EXPOSE 80 443
ENTRYPOINT ["/bin/sh", "/docker-entrypoint.sh"]
CMD ["nginx", "-g", "daemon off;"]
