#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: bash tools/transcode_sample_media.sh <source-video>" >&2
  exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_video="$(readlink -f "$1")"
output="${root}/assets/sample_demo_mjpeg_pcma.avi"
preview="${root}/assets/sample_demo_preview.jpg"
clip_start="40.5"
clip_duration="9.4"
video_width="1280"
video_height="720"
video_fps="15"
video_quality="25"
asset_budget_bytes="5130000"

command -v ffmpeg >/dev/null
command -v ffprobe >/dev/null
test -s "${source_video}"

ffmpeg -hide_banner -loglevel error -y \
  -ss "${clip_start}" -t "${clip_duration}" -i "${source_video}" \
  -vf "fps=${video_fps},scale=${video_width}:${video_height}:flags=lanczos" \
  -vcodec mjpeg -qscale "${video_quality}" -pix_fmt yuvj420p \
  -af "loudnorm=I=-18:TP=-3:LRA=7" \
  -acodec pcm_alaw -ar 8000 -ac 1 -f avi "${output}"

ffmpeg -hide_banner -loglevel error -y \
  -ss 4.7 -i "${output}" -vframes 1 "${preview}"

size="$(stat -c %s "${output}")"
video="$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,r_frame_rate,nb_frames \
  -of csv=p=0 "${output}")"
audio="$(ffprobe -v error -select_streams a:0 \
  -show_entries stream=codec_name,sample_rate,channels,nb_frames \
  -of csv=p=0 "${output}")"
duration="$(ffprobe -v error -show_entries format=duration \
  -of csv=p=0 "${output}")"
video_start="$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=start_time -of csv=p=0 "${output}")"
audio_start="$(ffprobe -v error -select_streams a:0 \
  -show_entries stream=start_time -of csv=p=0 "${output}")"
max_frame="$(ffprobe -v error -select_streams v:0 \
  -show_entries packet=size -of csv=p=0 "${output}" | sort -nr | head -n1)"

[[ "${size}" -le "${asset_budget_bytes}" ]]
[[ "${video}" == "mjpeg,1280,720,15/1,141" ]]
[[ "${audio}" == "pcm_alaw,8000,1,75200" ]]
[[ "${duration}" == "9.400000" ]]
[[ "${video_start}" == "0.000000" ]]
[[ "${audio_start}" == "0.000000" ]]
[[ "${max_frame}" -lt 104857 ]]

printf 'asset=%s bytes=%s duration=%ss max_frame=%s\n' \
  "${output}" "${size}" "${duration}" "${max_frame}"
sha256sum "${output}" "${preview}"
