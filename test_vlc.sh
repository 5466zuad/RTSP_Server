# 使用 ffplay (推荐，延迟低)
ffplay -fflags nobuffer -flags low_delay -rtsp_transport udp rtsp://127.0.0.1:8554/ &

# 或者如果你想用 VLC (取消下面注释即可):
# vlc rtsp://127.0.0.1:8554/ &
