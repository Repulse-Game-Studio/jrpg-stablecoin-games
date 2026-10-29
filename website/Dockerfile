# Builds the docs site and serves it with `docusaurus serve`. Build context is the
# repo root, because the site reads its pages from ../docs.
FROM node:22-alpine
WORKDIR /repo/website
COPY website/package.json website/package-lock.json ./
RUN npm ci
COPY docs /repo/docs
COPY website ./
ARG DOCS_URL=https://jrpg.com
ARG DOCS_BASE_URL=/stablecoinforgames/docs/
RUN DOCS_URL=$DOCS_URL DOCS_BASE_URL=$DOCS_BASE_URL npm run build
EXPOSE 3000
CMD ["npm", "run", "serve", "--", "--host", "0.0.0.0", "--port", "3000", "--no-open"]
