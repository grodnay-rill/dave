while read url path; do
    git submodule add $url $path
done << (vcs export --exact --output-repos dave.repos | grep -Eo 'https?://[^"]+')

