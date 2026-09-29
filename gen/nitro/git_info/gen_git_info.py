#!/usr/bin/env python3

import argparse
import os
import subprocess

def get_git_revision_short_hash() -> str:
    return subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD']).decode('ascii').strip()

def generate_header(libntr_dir, project_dir):
    currentDir = os.getcwd()
    guard_top = '\n'.join(['#ifndef GIT_INFO_H', '#define GIT_INFO_H', str()])
    os.chdir(libntr_dir)
    body = '#define SIM_LIBNTR_GIT_HASH "' + get_git_revision_short_hash() + '"\n'
    os.chdir(project_dir)
    body += '#define SIM_PROJECT_GIT_HASH "' + get_git_revision_short_hash() + '"\n'
    os.chdir(currentDir)
    guard_bottom = '\n'.join(['', '#endif // GIT_INFO_H', ''])
    return '\n'.join([guard_top, body, guard_bottom])

def main():
    args = parse_args()
    header = generate_header(args.libntr_dir, args.project_dir)
    with open(args.output, 'w') as out:
        out.write(header)


def parse_args():
    parser = argparse.ArgumentParser(description='git_info.h header generator')
    parser.add_argument('input', type=str)
    parser.add_argument('output', type=str)
    parser.add_argument('libntr_dir', type=str)
    parser.add_argument('project_dir', type=str)
    return parser.parse_args()


if __name__ == '__main__':
    main()
