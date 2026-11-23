from setuptools import find_packages, setup

package_name = 'careguard_server_bridge'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
        ('share/' + package_name + '/launch', ['launch/bridge.launch.py']),
    ],
    install_requires=['setuptools', 'websockets'],
    zip_safe=True,
    maintainer='careguard',
    maintainer_email='team@example.com',
    description='CareGuard central server ROS 2 <-> FastAPI WebSocket bridge',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'bridge_node = careguard_server_bridge.bridge_node:main',
        ],
    },
)
