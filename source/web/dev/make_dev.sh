#!/bin/sh
# Regenerates dev/index.html (browser preview with the mock backend) from index.html.
cd "$(dirname "$0")/.."
sed -e 's#href="uPlot.min.css"#href="../uPlot.min.css"#' -e 's#href="app.css"#href="../app.css"#' \
    -e 's#src="logo.svg"#src="../logo.svg"#g' \
    -e 's#<script src="uPlot.iife.min.js"></script>#<script src="mockdata.js"></script><script src="mock.js"></script><script src="../uPlot.iife.min.js"></script>#' \
    -e 's#<script src="app.js"></script>#<script src="../app.js"></script>#' index.html > dev/index.html
