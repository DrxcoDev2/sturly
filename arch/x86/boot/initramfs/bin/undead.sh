#!/bin/bash

# Author: Haitham Aouati
# GitHub: github.com/haithamaouati

# Colors
nc="\e[0m"
bold="\e[1m"
underlined="\e[4m"
bold_green="\e[1;32m"
bold_red="\e[1;31m"

OUTPUT_FILE="alive_proxies.txt"
TOTAL=0
ALIVE=0
DEAD=0

show_banner() {
    clear
    echo -e "${bold}"
    cat <<'EOF'
⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣠⣤⣤⣶⣶⣶⣶⣶⣶⣠⣤⣤⣀⣀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⣀⣴⣾⠿⠟⠛⠛⠉⠉⠉⠉⠉⠛⠛⠛⠿⠿⡿⠛⢿⣿⣷⣤
⠀⠀⠀⣠⡾⠟⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣸⣿⠉⠁
⠀⢀⡾⠋⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⡟⠀⠀
⢠⠟⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢠⣿⠁⠀⠀
⠋⠀⠀⠀⠀⠀⠀⠀⢀⣤⣤⣤⣶⣶⣆⢤⣤⣀⣀⠀⠀⠀⠀⣸⡟⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⢸⡿⠋⠙⢿⣿⣿⣿⣻⣿⣿⡇⠀⠀⠀⣿⠇⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⢸⡇⠀⢀⠐⢻⣿⣻⣷⡽⣿⣷⠀⠀⢸⣿⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠿⣃⠹⠿⠗⣿⣷⣻⣿⡽⣿⣧⠀⣼⡇⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⣴⡏⣁⢀⠲⣿⣿⣷⢻⣿⡽⣿⢀⣿⠁⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⣿⠁⣉⣡⣿⣿⣿⣏⣿⣿⣿⣼⡿⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⢰⡿⣿⣿⣿⣿⣿⣿⣿⣿⣽⣿⣿⣿⡇⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⢀⣿⣿⣿⣿⣿⣿⣿⣿⠟⠛⠛⠛⠛⠛⠆⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠈⠛⠘⠛⠛⠛⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
EOF
    echo -e "\n${bold} Undead${nc} — a tool to check if proxies are ALIVE or DEAD.${nc}\n"
    echo -e " Author: Haitham Aouati"
    echo -e " GitHub: ${underlined}github.com/haithamaouati${nc}\n"
}

show_help() {
    echo "Usage: $0 -p <proxy|file> [-t http|https|socks5] [-h]"
    echo
    echo "Options:"
    echo "  -p, --proxy    Proxy (ip:port) or file with proxies"
    echo "  -t, --type     Proxy type (http, https, socks5) [default: all]"
    echo -e "  -h, --help     Show this help message\n"
    exit 0
}

check_dependencies() {
    if ! command -v curl &> /dev/null; then
        echo -e "${bold_red}[!]${nc} Missing dependency: curl"
        echo "    Install it and try again."
        exit 1
    fi
}

try_proxy_type() {
    local proxy=$1
    local type=$2
    case $type in
        http) proto="http://" ;;
        https) proto="https://" ;;
        socks5) proto="socks5://" ;;
        *) return 1 ;;
    esac

    curl -s --proxy "${proto}${proxy}" --max-time 5 https://www.google.com > /dev/null
    return $?
}

check_proxy() {
    local proxy=$1
    ((TOTAL++))
    if [[ "$PROXY_TYPE" == "all" ]]; then
        for t in http https socks5; do
            try_proxy_type "$proxy" "$t"
            if [ $? -eq 0 ]; then
                echo -e "${bold_green}[+]${nc} $proxy is ${bold_green}ALIVE ($t)${nc}"
                echo "$proxy [$t]" >> "$OUTPUT_FILE"
                ((ALIVE++))
                return
            fi
        done
        echo -e "${bold_red}[-]${nc} $proxy is ${bold_red}DEAD${nc}"
        ((DEAD++))
    else
        try_proxy_type "$proxy" "$PROXY_TYPE"
        if [ $? -eq 0 ]; then
            echo -e "${bold_green}[+]${nc} $proxy is ${bold_green}ALIVE ($PROXY_TYPE)${nc}"
            echo "$proxy [$PROXY_TYPE]" >> "$OUTPUT_FILE"
            ((ALIVE++))
        else
            echo -e "${bold_red}[-]${nc} $proxy is ${bold_red}DEAD${nc}"
            ((DEAD++))
        fi
    fi
}

print_summary() {
    echo -e "\n${bold}[*]${nc} Total Proxies Checked: ${bold}$TOTAL${nc}"
    echo -e "${bold_green}[+]${nc} Alive Proxies:         ${bold_green}$ALIVE${nc}"
    echo -e "${bold_red}[-]${nc} Dead Proxies:          ${bold_red}$DEAD${nc}\n"
}

# Trap CTRL+C
trap ctrl_c INT
ctrl_c() {
    echo -e "\n\n${bold_red}[!] Interrupted${nc}"
    print_summary
    exit 1
}

# --- Start ---
show_banner
check_dependencies
> "$OUTPUT_FILE"

if [[ $# -eq 0 ]]; then
    show_help
fi

PROXY_TYPE="all"

while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--proxy)
            PROXY_INPUT=$2
            [[ -z "$PROXY_INPUT" ]] && echo -e "${bold_red}[!]${nc} Missing proxy argument after -p|--proxy" && exit 1
            shift 2
            ;;
        -t|--type)
            PROXY_TYPE=$2
            [[ -z "$PROXY_TYPE" ]] && echo -e "${bold_red}[!]${nc} Missing type argument after -t|--type" && exit 1
            shift 2
            ;;
        -h|--help)
            show_help
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            ;;
    esac
done

if [[ -f "$PROXY_INPUT" ]]; then
    echo -e "${bold}[*]${nc} Checking proxies from file: ${bold}$PROXY_INPUT${nc}\n"
    while IFS= read -r proxy; do
        [[ -z "$proxy" ]] && continue
        check_proxy "$proxy"
    done < "$PROXY_INPUT"
else
    echo -e "${bold}[*]${nc} Checking proxy: ${bold}$PROXY_INPUT${nc}\n"
    check_proxy "$PROXY_INPUT"
fi

print_summary