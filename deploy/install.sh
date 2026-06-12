#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$repo_root/build}"
install_prefix="${INSTALL_PREFIX:-/usr/local}"
config_dir="${CONFIG_DIR:-/etc/wh-repeater}"
state_dir="${STATE_DIR:-/var/lib/wh-repeater}"
web_dir="${WEB_DIR:-/var/www/wh-repeater}"
systemd_dir="${SYSTEMD_DIR:-/etc/systemd/system}"
nginx_available_dir="${NGINX_AVAILABLE_DIR:-/etc/nginx/sites-available}"
nginx_enabled_dir="${NGINX_ENABLED_DIR:-/etc/nginx/sites-enabled}"
ssl_dir="${SSL_DIR:-/etc/ssl/wh-repeater}"
service_name="${SERVICE_NAME:-wh-repeater.service}"
deploy_mode="${DEPLOY_MODE:-pc-gateway}"

want_nginx=1
want_service=1
start_service=1

usage() {
    cat <<EOF
usage: deploy/install.sh [--no-nginx] [--no-service] [--no-start]

Environment overrides:
  DEPLOY_MODE=pc-gateway|ts-gateway|local-transcode  initial config mode when no live config exists
  BUILD_DIR=$build_dir
  INSTALL_PREFIX=$install_prefix
  CONFIG_DIR=$config_dir
  WEB_DIR=$web_dir
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-nginx)
            want_nginx=0
            ;;
        --no-service)
            want_service=0
            start_service=0
            ;;
        --no-start)
            start_service=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
    shift
done

case "$deploy_mode" in
    pc-gateway|ts-gateway|local-transcode)
        ;;
    *)
        echo "deploy: DEPLOY_MODE must be pc-gateway, ts-gateway, or local-transcode" >&2
        exit 2
        ;;
esac

echo "deploy: configuring build in $build_dir"
cmake -S "$repo_root" -B "$build_dir"
cmake --build "$build_dir"

echo "deploy: installing binaries"
install -d -m 0755 "$install_prefix/bin"
install -m 0755 "$build_dir/wh-repeater" "$install_prefix/bin/wh-repeater"
if [[ -x "$build_dir/ts-gateway-inspect" ]]; then
    install -m 0755 "$build_dir/ts-gateway-inspect" "$install_prefix/bin/ts-gateway-inspect"
fi

echo "deploy: installing web UI"
install -d -m 0755 "$web_dir"
install -m 0644 "$repo_root/web/index.html" "$repo_root/web/styles.css" "$repo_root/web/app.js" "$web_dir/"

echo "deploy: ensuring config and state directories"
install -d -m 0755 "$config_dir" "$state_dir"
if [[ ! -f "$config_dir/wh-repeater.json" ]]; then
    tmp_config="$(mktemp)"
    sed "0,/\"mode\": \"[^\"]*\"/s//\"mode\": \"$deploy_mode\"/" "$repo_root/wh-repeater.json" > "$tmp_config"
    install -m 0644 "$tmp_config" "$config_dir/wh-repeater.json"
    rm -f "$tmp_config"
    echo "deploy: installed initial config with mode=$deploy_mode"
else
    echo "deploy: preserving existing $config_dir/wh-repeater.json"
fi

if [[ "$want_service" -eq 1 ]]; then
    echo "deploy: installing systemd service"
    install -d -m 0755 "$systemd_dir"
    install -m 0644 "$repo_root/deploy/systemd/wh-repeater.service" "$systemd_dir/$service_name"
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload
        systemctl enable "$service_name"
    fi
fi

if [[ "$want_nginx" -eq 1 ]]; then
    echo "deploy: installing nginx site"
    install -d -m 0755 "$nginx_available_dir" "$nginx_enabled_dir" "$ssl_dir"
    install -m 0644 "$repo_root/deploy/nginx/wh-repeater" "$nginx_available_dir/wh-repeater"
    if [[ -L "$nginx_enabled_dir/default" ]]; then
        rm -f "$nginx_enabled_dir/default"
        echo "deploy: disabled nginx default site"
    fi
    ln -sf "$nginx_available_dir/wh-repeater" "$nginx_enabled_dir/wh-repeater"

    if [[ ! -f "$ssl_dir/wh-repeater.crt" || ! -f "$ssl_dir/wh-repeater.key" ]]; then
        if command -v openssl >/dev/null 2>&1; then
            echo "deploy: creating self-signed nginx certificate"
            openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
                -keyout "$ssl_dir/wh-repeater.key" \
                -out "$ssl_dir/wh-repeater.crt" \
                -subj "/CN=wh-repeater"
            chmod 0600 "$ssl_dir/wh-repeater.key"
            chmod 0644 "$ssl_dir/wh-repeater.crt"
        else
            echo "deploy: openssl not found; install a certificate before reloading nginx" >&2
        fi
    fi

    if command -v nginx >/dev/null 2>&1; then
        nginx -t
        if command -v systemctl >/dev/null 2>&1; then
            systemctl reload nginx
        fi
    fi
fi

if [[ "$start_service" -eq 1 && "$want_service" -eq 1 ]] && command -v systemctl >/dev/null 2>&1; then
    echo "deploy: restarting $service_name"
    systemctl restart "$service_name"
fi

echo "deploy: complete"
