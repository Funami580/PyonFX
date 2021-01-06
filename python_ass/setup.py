from setuptools import setup

with open("README.md", 'r') as fh:
    long_description = fh.read()

setup(
    name='ass',
    version="0.5.1",
    description="A library for parsing and manipulating Advanced SubStation Alpha subtitle files.",
    long_description=long_description,
    long_description_content_type="text/markdown",
    author="Tony Young",
    # author_email="fichtefoll2@googlemail.com",
    keywords="ass subtitle substation alpha",
    packages=['ass'],
    url="http://github.com/chireiden/python-ass",
    license='MIT',
    classifiers=[
        'Development Status :: 4 - Beta',
        'Intended Audience :: Developers',
        'Operating System :: OS Independent',
        'Programming Language :: Python :: 3',
        'Topic :: Multimedia :: Video',
        'Topic :: Software Development :: Libraries',
        'Topic :: Text Processing :: Markup',
    ],
    install_requires=["setuptools"],
    zip_safe=True,
)
