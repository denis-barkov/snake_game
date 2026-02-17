project           = "snake"
owner             = "denis_pm"
domain_name       = "terrariumsnake.com"
route53_zone_name = "terrariumsnake.com"

# Update to a real mailbox you control before apply.
letsencrypt_email = "admin@terrariumsnake.com"

# Repository used by EC2 bootstrap script.
app_git_repo     = "https://github.com/denis-progman/snake_game.git"
app_build_target = "api/snake_server.cpp"
