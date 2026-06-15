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
preview_service_name="${PREVIEW_SERVICE_NAME:-wh-preview.service}"
deploy_mode="${DEPLOY_MODE:-pc-gateway}"
deploy_target="${DEPLOY_TARGET:-}"

want_nginx=1
want_web=1
want_service=1
start_service=1

usage() {
    cat <<EOF
usage: deploy/install.sh [--no-web] [--no-nginx] [--no-service] [--no-start]

Environment overrides:
  DEPLOY_TARGET=pc-gateway|pi-gateway|legacy        installed service/config layout
  DEPLOY_MODE=pc-gateway|ts-gateway|local-transcode  initial config mode when no live config exists
  BUILD_DIR=$build_dir
  INSTALL_PREFIX=$install_prefix
  CONFIG_DIR=$config_dir
  WEB_DIR=$web_dir
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-web)
            want_web=0
            want_nginx=0
            ;;
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

if [[ -z "$deploy_target" ]]; then
    case "$deploy_mode" in
        ts-gateway)
            deploy_target="pi-gateway"
            ;;
        pc-gateway)
            deploy_target="pc-gateway"
            ;;
        local-transcode)
            deploy_target="legacy"
            ;;
    esac
fi

case "$deploy_target" in
    pc-gateway)
        service_name="${SERVICE_NAME:-wh-pc-gateway.service}"
        config_dir="${CONFIG_DIR:-/etc/wh-pc-gateway}"
        state_dir="${STATE_DIR:-/var/lib/wh-pc-gateway}"
        deploy_mode="${DEPLOY_MODE:-pc-gateway}"
        ;;
    pi-gateway)
        service_name="${SERVICE_NAME:-wh-pi-gateway.service}"
        config_dir="${CONFIG_DIR:-/etc/wh-pi-gateway}"
        state_dir="${STATE_DIR:-/var/lib/wh-pi-gateway}"
        deploy_mode="${DEPLOY_MODE:-ts-gateway}"
        want_web=0
        want_nginx=0
        preview_service_name=""
        ;;
    legacy)
        ;;
    *)
        echo "deploy: DEPLOY_TARGET must be pc-gateway, pi-gateway, or legacy" >&2
        exit 2
        ;;
esac

case "$deploy_mode" in
    pc-gateway|ts-gateway|local-transcode)
        ;;
    *)
        echo "deploy: DEPLOY_MODE must be pc-gateway, ts-gateway, or local-transcode" >&2
        exit 2
        ;;
esac

case "$deploy_target:$deploy_mode" in
    pc-gateway:pc-gateway|pi-gateway:ts-gateway|legacy:*)
        ;;
    *)
        echo "deploy: DEPLOY_TARGET=$deploy_target expects DEPLOY_MODE=$(
            case "$deploy_target" in
                pc-gateway) echo pc-gateway ;;
                pi-gateway) echo ts-gateway ;;
                *) echo local-transcode ;;
            esac
        )" >&2
        exit 2
        ;;
esac

echo "deploy: target=$deploy_target mode=$deploy_mode service=$service_name"
echo "deploy: config=$config_dir state=$state_dir"
echo "deploy: configuring build in $build_dir"
cmake -S "$repo_root" -B "$build_dir"
case "$deploy_target" in
    pc-gateway)
        build_targets=(wh-pc-gateway)
        install_binaries=(wh-pc-gateway)
        ;;
    pi-gateway)
        build_targets=(wh-pi-gateway)
        install_binaries=(wh-pi-gateway)
        ;;
    legacy)
        build_targets=(wh-repeater)
        install_binaries=(wh-repeater)
        ;;
esac
if [[ "$deploy_target" != "pc-gateway" ]]; then
    build_targets+=(ts-gateway-inspect)
fi
for target in "${build_targets[@]}"; do
    cmake --build "$build_dir" --target "$target"
done

echo "deploy: installing binaries"
install -d -m 0755 "$install_prefix/bin"
for binary in "${install_binaries[@]}"; do
    if [[ -x "$build_dir/$binary" ]]; then
        install -m 0755 "$build_dir/$binary" "$install_prefix/bin/$binary"
    fi
done
if [[ -x "$build_dir/ts-gateway-inspect" ]]; then
    install -m 0755 "$build_dir/ts-gateway-inspect" "$install_prefix/bin/ts-gateway-inspect"
fi
if [[ -f "$repo_root/tools/render-slate-html" ]]; then
    install -m 0755 "$repo_root/tools/render-slate-html" "$install_prefix/bin/render-slate-html"
fi

if [[ "$want_web" -eq 1 ]]; then
    echo "deploy: installing web UI"
    install -d -m 0755 "$web_dir"
    install -m 0644 "$repo_root/web/index.html" "$repo_root/web/styles.css" "$repo_root/web/app.js" "$web_dir/"
else
    echo "deploy: skipping web UI for $deploy_target"
fi

echo "deploy: ensuring config and state directories"
install -d -m 0755 "$config_dir" "$state_dir"
if [[ -d "$repo_root/slates/default" ]]; then
    echo "deploy: installing default slate templates"
    install -d -m 0755 "$config_dir/slates"
    for slate_file in "$repo_root"/slates/default/*; do
        slate_name="$(basename "$slate_file")"
        if [[ ! -f "$config_dir/slates/$slate_name" ]]; then
            install -m 0644 "$slate_file" "$config_dir/slates/$slate_name"
        fi
    done
fi
config_file="$config_dir/config.json"
if [[ "$deploy_target" == "legacy" ]]; then
    config_file="$config_dir/wh-repeater.json"
fi
if [[ ! -f "$config_file" ]]; then
    tmp_config="$(mktemp)"
    seed_config="$repo_root/wh-repeater.json"
    if [[ "$deploy_target" != "legacy" && -f /etc/wh-repeater/wh-repeater.json ]]; then
        seed_config="/etc/wh-repeater/wh-repeater.json"
        echo "deploy: migrating initial config from $seed_config"
    fi
    sed "0,/\"mode\": \"[^\"]*\"/s//\"mode\": \"$deploy_mode\"/" "$seed_config" > "$tmp_config"
    install -m 0644 "$tmp_config" "$config_file"
    rm -f "$tmp_config"
    echo "deploy: installed initial config with mode=$deploy_mode"
else
    echo "deploy: preserving existing $config_file"
fi

if [[ "$want_service" -eq 1 ]]; then
    echo "deploy: installing systemd service"
    install -d -m 0755 "$systemd_dir"
    case "$deploy_target" in
        pc-gateway)
            install -m 0644 "$repo_root/deploy/systemd/wh-pc-gateway.service" "$systemd_dir/$service_name"
            install -m 0644 "$repo_root/deploy/systemd/wh-preview.service" "$systemd_dir/$preview_service_name"
            ;;
        pi-gateway)
            install -m 0644 "$repo_root/deploy/systemd/wh-pi-gateway.service" "$systemd_dir/$service_name"
            ;;
        legacy)
            install -m 0644 "$repo_root/deploy/systemd/wh-repeater.service" "$systemd_dir/$service_name"
            install -m 0644 "$repo_root/deploy/systemd/wh-preview.service" "$systemd_dir/$preview_service_name"
            ;;
    esac
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload
        systemctl enable "$service_name"
        if [[ "$deploy_target" != "legacy" && "$service_name" != "wh-repeater.service" ]]; then
            systemctl disable wh-repeater.service >/dev/null 2>&1 || true
            systemctl stop wh-repeater.service >/dev/null 2>&1 || true
        fi
        if [[ -n "$preview_service_name" ]]; then
            systemctl disable "$preview_service_name" >/dev/null 2>&1 || true
        fi
    fi
fi

if [[ "$deploy_target" == "pi-gateway" ]]; then
    if [[ -L "$nginx_enabled_dir/wh-repeater" ]]; then
        rm -f "$nginx_enabled_dir/wh-repeater"
        echo "deploy: disabled Pi nginx wh-repeater site"
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl reload nginx >/dev/null 2>&1 || true
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
